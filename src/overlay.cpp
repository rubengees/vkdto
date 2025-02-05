/*
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#if defined(__GNUC__) && __GNUC__ >= 4
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#elif defined(__SUNPRO_C) && (__SUNPRO_C >= 0x590)
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#else
#define VK_LAYER_EXPORT
#endif

#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <wchar.h>
#include <iconv.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdio>
#include <iostream>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "imgui.h"
#include "hash_table.h"
#include "list.h"
#include "ralloc.h"
#include "simple_mtx.h"

#include "vk_enum_to_str.h"
#include "vk_util.h"

#include "hashtext_fmt.h"

/* Mapped from VkInstace/VkPhysicalDevice */
struct instance_data {
   struct vk_instance_dispatch_table vtable;
   VkInstance instance;

   bool first_line_printed;
};

/* Mapped from VkDevice */
struct queue_data;
struct device_data {
   struct instance_data *instance;

   PFN_vkSetDeviceLoaderData set_device_loader_data;

   struct vk_device_dispatch_table vtable;
   VkPhysicalDevice physical_device;
   VkDevice device;

   VkPhysicalDeviceProperties properties;

   struct queue_data *graphic_queue;

   struct queue_data **queues;
   uint32_t n_queues;
};

/* Mapped from VkQueue */
struct queue_data {
   struct device_data *device;

   VkQueue queue;
   VkQueueFlags flags;
   uint32_t family_index;
};

struct overlay_draw {
   struct list_head link;

   VkCommandBuffer command_buffer;

   VkSemaphore cross_engine_semaphore;

   VkSemaphore semaphore;
   VkFence fence;

   VkBuffer vertex_buffer;
   VkDeviceMemory vertex_buffer_mem;
   VkDeviceSize vertex_buffer_size;

   VkBuffer index_buffer;
   VkDeviceMemory index_buffer_mem;
   VkDeviceSize index_buffer_size;
};

/* Mapped from VkSwapchainKHR */
struct swapchain_data {
   struct device_data *device;

   VkSwapchainKHR swapchain;
   unsigned width, height;
   VkFormat format;

   uint32_t n_images;
   VkImage *images;
   VkImageView *image_views;
   VkFramebuffer *framebuffers;

   VkRenderPass render_pass;

   VkDescriptorPool descriptor_pool;
   VkDescriptorSetLayout descriptor_layout;
   VkDescriptorSet descriptor_set;

   VkSampler font_sampler;

   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;

   VkCommandPool command_pool;

   struct list_head draws; /* List of struct overlay_draw */

   bool font_uploaded;
   VkImage font_image;
   VkImageView font_image_view;
   VkDeviceMemory font_mem;
   VkBuffer upload_font_buffer;
   VkDeviceMemory upload_font_buffer_mem;
   ImFont *ubuntu_mon_reg,
	  *ubuntu_mon_bold;

   /**/
   ImGuiContext* imgui_context;
   ImVec2 window_size;
};

static struct hash_table_u64 *vk_object_to_data = NULL;
static simple_mtx_t vk_object_to_data_mutex = _SIMPLE_MTX_INITIALIZER_NP;

thread_local ImGuiContext* __MesaImGui;

static inline void ensure_vk_object_map(void)
{
   if (!vk_object_to_data)
      vk_object_to_data = _mesa_hash_table_u64_create(NULL);
}

#define HKEY(obj) ((uint64_t)(obj))
#define FIND(type, obj) ((type *)find_object_data(HKEY(obj)))

static void *find_object_data(uint64_t obj)
{
   simple_mtx_lock(&vk_object_to_data_mutex);
   ensure_vk_object_map();
   void *data = _mesa_hash_table_u64_search(vk_object_to_data, obj);
   simple_mtx_unlock(&vk_object_to_data_mutex);
   return data;
}

static void map_object(uint64_t obj, void *data)
{
   simple_mtx_lock(&vk_object_to_data_mutex);
   ensure_vk_object_map();
   _mesa_hash_table_u64_insert(vk_object_to_data, obj, data);
   simple_mtx_unlock(&vk_object_to_data_mutex);
}

static void unmap_object(uint64_t obj)
{
   simple_mtx_lock(&vk_object_to_data_mutex);
   _mesa_hash_table_u64_remove(vk_object_to_data, obj);
   simple_mtx_unlock(&vk_object_to_data_mutex);
}

/**/

#define VK_CHECK(expr) \
   do { \
      VkResult __result = (expr); \
      if (__result != VK_SUCCESS) { \
         fprintf(stderr, "'%s' line %i failed with %s\n", \
                 #expr, __LINE__, vk_Result_to_str(__result)); \
      } \
   } while (0)

/**/

static VkLayerInstanceCreateInfo *get_instance_chain_info(const VkInstanceCreateInfo *pCreateInfo,
                                                          VkLayerFunction func)
{
   vk_foreach_struct(item, pCreateInfo->pNext) {
      if (item->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
          ((VkLayerInstanceCreateInfo *) item)->function == func)
         return (VkLayerInstanceCreateInfo *) item;
   }
   unreachable("instance chain info not found");
   return NULL;
}

static VkLayerDeviceCreateInfo *get_device_chain_info(const VkDeviceCreateInfo *pCreateInfo,
                                                      VkLayerFunction func)
{
   vk_foreach_struct(item, pCreateInfo->pNext) {
      if (item->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
          ((VkLayerDeviceCreateInfo *) item)->function == func)
         return (VkLayerDeviceCreateInfo *)item;
   }
   unreachable("device chain info not found");
   return NULL;
}

/**/

static struct instance_data *new_instance_data(VkInstance instance)
{
   struct instance_data *data = rzalloc(NULL, struct instance_data);
   data->instance = instance;
   map_object(HKEY(data->instance), data);
   return data;
}

static void destroy_instance_data(struct instance_data *data)
{
   unmap_object(HKEY(data->instance));
   ralloc_free(data);
}

static void instance_data_map_physical_devices(struct instance_data *instance_data,
                                               bool map)
{
   uint32_t physicalDeviceCount = 0;
   instance_data->vtable.EnumeratePhysicalDevices(instance_data->instance,
                                                  &physicalDeviceCount,
                                                  NULL);

   VkPhysicalDevice *physicalDevices = (VkPhysicalDevice *) malloc(sizeof(VkPhysicalDevice) * physicalDeviceCount);
   instance_data->vtable.EnumeratePhysicalDevices(instance_data->instance,
                                                  &physicalDeviceCount,
                                                  physicalDevices);

   for (uint32_t i = 0; i < physicalDeviceCount; i++) {
      if (map)
         map_object(HKEY(physicalDevices[i]), instance_data);
      else
         unmap_object(HKEY(physicalDevices[i]));
   }

   free(physicalDevices);
}

/**/
static struct device_data *new_device_data(VkDevice device, struct instance_data *instance)
{
   struct device_data *data = rzalloc(NULL, struct device_data);
   data->instance = instance;
   data->device = device;
   map_object(HKEY(data->device), data);
   return data;
}

static struct queue_data *new_queue_data(VkQueue queue,
                                         const VkQueueFamilyProperties *family_props,
                                         uint32_t family_index,
                                         struct device_data *device_data)
{
   struct queue_data *data = rzalloc(device_data, struct queue_data);
   data->device = device_data;
   data->queue = queue;
   data->flags = family_props->queueFlags;
   data->family_index = family_index;
   map_object(HKEY(data->queue), data);

   if (data->flags & VK_QUEUE_GRAPHICS_BIT)
      device_data->graphic_queue = data;

   return data;
}

static void destroy_queue(struct queue_data *data)
{
   unmap_object(HKEY(data->queue));
   ralloc_free(data);
}

static void device_map_queues(struct device_data *data,
                              const VkDeviceCreateInfo *pCreateInfo)
{
   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++)
      data->n_queues += pCreateInfo->pQueueCreateInfos[i].queueCount;
   data->queues = ralloc_array(data, struct queue_data *, data->n_queues);

   struct instance_data *instance_data = data->instance;
   uint32_t n_family_props;
   instance_data->vtable.GetPhysicalDeviceQueueFamilyProperties(data->physical_device,
                                                                &n_family_props,
                                                                NULL);
   VkQueueFamilyProperties *family_props =
      (VkQueueFamilyProperties *)malloc(sizeof(VkQueueFamilyProperties) * n_family_props);
   instance_data->vtable.GetPhysicalDeviceQueueFamilyProperties(data->physical_device,
                                                                &n_family_props,
                                                                family_props);

   uint32_t queue_index = 0;
   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      for (uint32_t j = 0; j < pCreateInfo->pQueueCreateInfos[i].queueCount; j++) {
         VkQueue queue;
         data->vtable.GetDeviceQueue(data->device,
                                     pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex,
                                     j, &queue);

         VK_CHECK(data->set_device_loader_data(data->device, queue));

         data->queues[queue_index++] =
            new_queue_data(queue, &family_props[pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex],
                           pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex, data);
      }
   }

   free(family_props);
}

static void device_unmap_queues(struct device_data *data)
{
   for (uint32_t i = 0; i < data->n_queues; i++)
      destroy_queue(data->queues[i]);
}

static void destroy_device_data(struct device_data *data)
{
   unmap_object(HKEY(data->device));
   ralloc_free(data);
}

/**/
static struct swapchain_data *new_swapchain_data(VkSwapchainKHR swapchain,
                                                 struct device_data *device_data)
{
   struct swapchain_data *data = rzalloc(NULL, struct swapchain_data);
   data->device = device_data;
   data->swapchain = swapchain;
   // initialize the window size with
   // some default values
   data->window_size = ImVec2(128, 128);
   list_inithead(&data->draws);
   map_object(HKEY(data->swapchain), data);
   return data;
}

static void destroy_swapchain_data(struct swapchain_data *data)
{
   unmap_object(HKEY(data->swapchain));
   ralloc_free(data);
}

struct overlay_draw *get_overlay_draw(struct swapchain_data *data)
{
   struct device_data *device_data = data->device;
   struct overlay_draw *draw = list_is_empty(&data->draws) ?
      NULL : list_first_entry(&data->draws, struct overlay_draw, link);

   VkSemaphoreCreateInfo sem_info = {};
   sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

   if (draw && device_data->vtable.GetFenceStatus(device_data->device, draw->fence) == VK_SUCCESS) {
      list_del(&draw->link);
      VK_CHECK(device_data->vtable.ResetFences(device_data->device,
                                               1, &draw->fence));
      list_addtail(&draw->link, &data->draws);
      return draw;
   }

   draw = rzalloc(data, struct overlay_draw);

   VkCommandBufferAllocateInfo cmd_buffer_info = {};
   cmd_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cmd_buffer_info.commandPool = data->command_pool;
   cmd_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cmd_buffer_info.commandBufferCount = 1;
   VK_CHECK(device_data->vtable.AllocateCommandBuffers(device_data->device,
                                                       &cmd_buffer_info,
                                                       &draw->command_buffer));
   VK_CHECK(device_data->set_device_loader_data(device_data->device,
                                                draw->command_buffer));


   VkFenceCreateInfo fence_info = {};
   fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
   VK_CHECK(device_data->vtable.CreateFence(device_data->device,
                                            &fence_info,
                                            NULL,
                                            &draw->fence));

   VK_CHECK(device_data->vtable.CreateSemaphore(device_data->device, &sem_info,
                                                NULL, &draw->semaphore));
   VK_CHECK(device_data->vtable.CreateSemaphore(device_data->device, &sem_info,
                                                NULL, &draw->cross_engine_semaphore));

   list_addtail(&draw->link, &data->draws);

   return draw;
}

namespace vkdto {
	enum pos {
		TL = 0,
		TC,
		TR,
		BL,
		BC,
		BR
	};

	namespace debug {
		FILE	*log = 0;

		std::string get_ts(void) {
			using namespace std::chrono;
			const auto	tp = high_resolution_clock::now();
			const auto	tp_tm = time_point_cast<system_clock::duration>(tp);
			const auto	tm_t = system_clock::to_time_t(tp_tm);
			struct tm	res = {0};
			localtime_r(&tm_t, &res);
			char		tm_fmt[32],
					tm_buf[32];
			std::sprintf(tm_fmt, "%%Y-%%m-%%dT%%H:%%M:%%S.%03i", static_cast<int>(duration_cast<milliseconds>(tp.time_since_epoch()).count()%1000));
			std::strftime(tm_buf, sizeof(tm_buf), tm_fmt, &res);

			return std::string(tm_buf);
		}
	}

	namespace opt {
		const char	*PARAM_POS = "pos",
				*PARAM_FONT_SIZE = "font_size",
				*PARAM_MARGIN = "margin",
				*PARAM_ALPHA = "alpha";

		const char	*input_file = 0;
		size_t		buf_sz = 1024*16; //16K buffers
		int		ms_update_wait = 250;
		float		font_size = 20.0f;
		float		font_x_size = -1.0f;
		float		margin = 10.0f;
		float		alpha = 1.0f;
		pos		ol_pos = TL;
	}

	void set_ol_pos(const std::string& v) {
		if(v == "tl") opt::ol_pos = pos::TL;
		else if(v == "tc") opt::ol_pos = pos::TC;
		else if(v == "tr") opt::ol_pos = pos::TR;
		else if(v == "bl") opt::ol_pos = pos::BL;
		else if(v == "bc") opt::ol_pos = pos::BC;
		else if(v == "br") opt::ol_pos = pos::BR;
	}

	void load_opt(void) {
		static std::atomic<bool>	first_run(true);
		bool				exp_val = true;
		if(!first_run || !first_run.compare_exchange_strong(exp_val, false))
			return;

		// debug file
		const char* dbg_file = std::getenv("VKDTO_DEBUG_LOG");
		if(dbg_file) {
			debug::log = std::fopen(dbg_file, "a");
			if(debug::log) {
				std::fprintf(debug::log, "%s VKDTO_DEBUG_LOG started\n", debug::get_ts().c_str());
			}
		}

		// file to be loaded
		opt::input_file = std::getenv("VKDTO_FILE");

		// scan the options
		// format is
		// <param>=<value>:<param>=<value>:...
		const char	*opt_str = std::getenv("VKDTO_OPT");
		if(opt_str) {
			while(*opt_str) {
				// search for ':'
				// and for '='
				const char		*next_p = std::strchr(opt_str, ':'),
							*cur_sep = std::strchr(opt_str, '=');
				if(next_p && (!cur_sep || (cur_sep > next_p))) {
					opt_str = next_p+1;
					continue;
				} else if (!cur_sep) {
					break;
				}
				const std::string	cur_p(opt_str, cur_sep),
							cur_v(next_p ? std::string(cur_sep+1, next_p) : std::string(cur_sep+1));
				// decide which parameter to fill
				if(cur_p == opt::PARAM_POS) set_ol_pos(cur_v);
				else if(cur_p == opt::PARAM_FONT_SIZE) {
					const double	val = std::atof(cur_v.c_str());
					if(val > 0.0) opt::font_size = val;
				} else if(cur_p == opt::PARAM_MARGIN) {
					const double	val = std::atof(cur_v.c_str());
					if(val >= 0.0) opt::margin = val;
				} else if(cur_p == opt::PARAM_ALPHA) {
					const double	val = std::atof(cur_v.c_str());
					if(val >= 0.0) opt::alpha = val;
					if(opt::alpha > 1.0f) opt::alpha = 1.0f;
				}

				// set the next opt_str
				// and carry on
				opt_str = next_p ? (next_p + 1) : (opt_str + std::strlen(opt_str));
			}
		}
	}

	void set_font_x_size(ImFont* regular, ImFont* bold) {
		const static char	utf8_latin[] = 
		"abcdefghijklmnopqrstuvwxyz0123456789"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ~`!@#$%^&*()_+-="
		"[]\\{}|;':\",./<>?";
		ImGui::PushFont(regular);
		const auto	sz_reg = ImGui::CalcTextSize(&utf8_latin[0], &utf8_latin[0] + sizeof(utf8_latin)/sizeof(utf8_latin[0]), false, 0.0);
		ImGui::PopFont();
		ImGui::PushFont(bold);
		const auto	sz_bold = ImGui::CalcTextSize(&utf8_latin[0], &utf8_latin[0] + sizeof(utf8_latin)/sizeof(utf8_latin[0]), false, 0.0);
		ImGui::PopFont();
		// ensure the fonts have the same dimensions
		assert((sz_reg.x == sz_bold.x) && (sz_reg.y == sz_bold.y));
		// take an average for x size
		opt::font_x_size = sz_reg.x / (sizeof(utf8_latin)/sizeof(utf8_latin[0]));
	}

	std::string to_utf8(const wchar_t* beg, const wchar_t* end) {
		const size_t	bytes_sz = 4*(end-beg);
		std::string	rv;
		rv.resize(bytes_sz);
		auto		conv = iconv_open("UTF-8", "WCHAR_T");
		if(((void*)-1) == conv)
			return "";
		char		*pIn = (char*)beg,
				*pOut = (char*)&rv[0];
		size_t		sIn = sizeof(wchar_t)*(end-beg),
				sOut = bytes_sz;
		const int	cv = iconv(conv, &pIn, &sIn, &pOut, &sOut);
		iconv_close(conv);
		if(-1 == cv) return "";
		else rv.resize(bytes_sz - sOut);
		return rv;
	}

	std::atomic<wchar_t*>	data_buffer(0);

	void th_data_load(void) {
		std::vector<wchar_t>	bufs[2];
		bufs[0].resize(opt::buf_sz);
		bufs[1].resize(opt::buf_sz);

		// I know this may suffer from 'tearing' :)
		size_t			cur_idx = 0;
		struct stat		latest_stat = {0}; 
		do {
			// sleep for a bit
			struct timespec	ts = { opt::ms_update_wait/1000, (opt::ms_update_wait%1000)*1000000 };
			nanosleep(&ts, 0);
			const size_t	next_idx = (cur_idx+1)%(sizeof(bufs)/sizeof(bufs[0]));
			std::swprintf(&bufs[next_idx][0], opt::buf_sz,
					L"Couldn't load contents of '%s'.\nPlease check variable 'VKDTO_FILE' refers to a valid file",
					(opt::input_file) ? opt::input_file : "");
			struct stat	cur_stats;
			if(opt::input_file && !stat(opt::input_file, &cur_stats)) {
				if(cur_stats.st_ino == latest_stat.st_ino && cur_stats.st_ctime == latest_stat.st_ctime) {
					latest_stat = cur_stats;
					continue;
				}

				int	fd = open(opt::input_file, O_RDONLY);
				if(-1 != fd) {
					int 	rb = -1,
						tot_rb = 0;
					char	*buf_start = (char*)&bufs[next_idx][0];
					do {
						// read as much as we can
						rb = read(fd, buf_start + tot_rb, (opt::buf_sz)*sizeof(wchar_t) - tot_rb);
						if(rb > 0)
							tot_rb += rb;
					} while(rb > 0);
					close(fd);
					// data should be wchar_t divisible
					// but we truncate anyway
					const auto	last_wchar = tot_rb/sizeof(wchar_t);
					bufs[next_idx][last_wchar] = L'\0';
				}
			}
			data_buffer.store(&bufs[next_idx][0]);
			cur_idx = next_idx;
		} while(true);
	}

	const wchar_t* sample_data(void) {
		static const wchar_t	data__[] = 
		L"linux-hunter 0.0.7              (1001/   0/   0 w/u/s)\n"
		"SessionId:[Pc8Ec&JMnaQ2] Host:[Emetta]\n"
		"\n"
		"Player Name                     Id  Damage    %       \n"
		"Left the session                0         8223   21.20\n"
		"Legolas                         1        13063   33.67\n"
		"Emetta                          2        12798   32.99\n"
		"Umi                             3         4712   12.15\n"
		"Total                                    38796  100.00\n"
		"\n"
		"Monster Name                    HP            %       \n"
		"Zinogre                           12489/ 64068   19.49\n"
		"Fulgur Anjanath                   43884/ 48796   89.93\n"
		"Banbaro                           41945/ 42126   99.57";

		static std::atomic<bool>	first_run(true);
		bool				exp_val = true;
		if(first_run && first_run.compare_exchange_strong(exp_val, false)) {
			std::thread	t1(th_data_load);
			t1.detach();
		}

		const wchar_t	*data = (data_buffer.load()) ? data_buffer.load() : data__;

		return data;
	}

	void position_layer(struct swapchain_data *data) {
		if(debug::log) {
			std::fprintf(debug::log,
					"%s %s swapchain_data %p data->width %d data->height %d\n",
					debug::get_ts().c_str(),
					__FUNCTION__,
					data,
					data->width,
					data->height);
			std::fflush(debug::log);
		}


		ImGui::SetNextWindowBgAlpha(opt::alpha*0.5f);
		ImGui::SetNextWindowSize(data->window_size, ImGuiCond_Always);
		switch (opt::ol_pos) {
		default:
		case pos::TL:
			ImGui::SetNextWindowPos(ImVec2(opt::margin, opt::margin), ImGuiCond_Always);
			break;
		case pos::TC:
			ImGui::SetNextWindowPos(ImVec2(0.5*(data->width - data->window_size.x), opt::margin),
					      ImGuiCond_Always);
			break;
		case pos::TR:
			ImGui::SetNextWindowPos(ImVec2(data->width - data->window_size.x - opt::margin, opt::margin),
					      ImGuiCond_Always);
			break;
		case pos::BL:
			ImGui::SetNextWindowPos(ImVec2(opt::margin, data->height - data->window_size.y - opt::margin),
					      ImGuiCond_Always);
			break;
		case pos::BC:
			ImGui::SetNextWindowPos(ImVec2(0.5*(data->width - data->window_size.x),
						data->height - data->window_size.y - opt::margin),
					      ImGuiCond_Always);
			break;
		case pos::BR:
			ImGui::SetNextWindowPos(ImVec2(data->width - data->window_size.x - opt::margin,
						     data->height - data->window_size.y - opt::margin),
					      ImGuiCond_Always);
			break;
		}
	}


	// Utility function to process metadata attributes
	void draw_metadata(const uint32_t md, struct swapchain_data *sc_data, bool& draw_white_bg) {
		switch(md) {
		case ht_fmt::BOLD_ON: {
			ImGui::PushFont(sc_data->ubuntu_mon_bold);
		} break;
		case ht_fmt::BOLD_OFF: {
			ImGui::PopFont();
		} break;
		case ht_fmt::DIM_ON: {
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opt::alpha*0.5f);
		} break;
		case ht_fmt::DIM_OFF: {
			ImGui::PopStyleVar();
		} break;
		case ht_fmt::BLUE_ON: {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0, 0.0, 1.0, 1.0));
		} break;
		case ht_fmt::BLUE_OFF: {
			ImGui::PopStyleColor();
		} break;
		case ht_fmt::MAGENTA_ON: {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0, 0.0, 1.0, 1.0));
		} break;
		case ht_fmt::MAGENTA_OFF: {
			ImGui::PopStyleColor();
		} break;
		case ht_fmt::YELLOW_ON: {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0, 1.0, 1.0, 1.0));
		} break;
		case ht_fmt::YELLOW_OFF: {
			ImGui::PopStyleColor();
		} break;
		case ht_fmt::GREEN_ON: {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0, 1.0, 0.0, 1.0));
		} break;
		case ht_fmt::GREEN_OFF: {
			ImGui::PopStyleColor();
		} break;
		case ht_fmt::REVERSE_ON: {
			draw_white_bg = true;
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0, 0.0, 0.0, 1.0));
		} break;
		case ht_fmt::REVERSE_OFF: {
			ImGui::PopStyleColor();
			draw_white_bg = false;
		} break;
		default:
			break;
		}
	}

	// returns the max number of chars written
	// for any row
	struct rect2s {
		size_t	x,
			y;
	};

	rect2s draw_data(const wchar_t* data, struct swapchain_data *sc_data) {
		rect2s		rv = {0, 0};
		const wchar_t	*cur_data = data,
				*next_line = wcschr(cur_data, L'\n');
		bool		draw_white_bg = false;

		auto fn_print_row = [&sc_data, &draw_white_bg](const wchar_t* b, const wchar_t* e) -> size_t {
			const static wchar_t	ESC_CHAR = L'#';
			const wchar_t*		next_esc = wcschr(b, ESC_CHAR);
			float			cur_pos = -1.0;

			auto fn_set_cur_pos = [&cur_pos](void) -> void {
				const auto	c_cur_pos = ImGui::GetCursorPosX();
				if(c_cur_pos > cur_pos)
					cur_pos = c_cur_pos;
			};

			if(e == b) {
				ImGui::Text("%s", "");
				return 0;
			}

			auto fn_draw_bg = [&draw_white_bg](const std::string& utf8s) {
				if(!draw_white_bg)
					return;

				ImDrawList*		dl = ImGui::GetWindowDrawList();
				const ImVec2		text_pos = ImGui::GetCursorScreenPos(),
					      		text_size = ImGui::CalcTextSize(&(*utf8s.begin()), &(*utf8s.end()), false, 0.0);
				const static ImU32	col32 = 0xFFFFFFFF;
				dl->AddRectFilled(text_pos, ImVec2(text_pos.x + text_size.x, text_pos.y + text_size.y), col32, 0.0f, ImDrawCornerFlags_All);
			};

			while(next_esc && (next_esc < e)) {
				// if we have to draw white background
				const auto	utf8s(to_utf8(b, next_esc));
				fn_draw_bg(utf8s);
				// print what is between b and next_esc (reset the line)
				ImGui::Text("%s", utf8s.c_str()); ImGui::SameLine(0.0f, 0.0f); fn_set_cur_pos();
				// there should be at least 2 valid wchar_t at
				// next_esc pointer: itself and the one after
				// if it's not the case, we have malformed input
				if(next_esc+2 > e)
					return (size_t)cur_pos;
				// in case is fimply to escape the '#', print it
				if(next_esc[1] == ESC_CHAR) {
					fn_draw_bg("#");
					ImGui::Text("%s", "#"); ImGui::SameLine(0.0f, 0.0f); fn_set_cur_pos();
				} else {
					draw_metadata((uint32_t)next_esc[1], sc_data, draw_white_bg);
				}
				// Loop around
				b = next_esc+2;
				next_esc = wcschr(b, ESC_CHAR);
			}
			if(e > b) {
				const auto	utf8s(to_utf8(b, e));
				fn_draw_bg(utf8s);
				ImGui::Text("%s", utf8s.c_str()); ImGui::SameLine(0.0f, 0.0f); fn_set_cur_pos();
			}
			// no matter what happens, print a newline...
			ImGui::Text("%s", "");
			return (size_t)cur_pos;
		};

		// set alpha value for all text
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opt::alpha);

		while(next_line) {
			const auto	cur_rv = fn_print_row(cur_data, next_line);
			if(cur_rv > rv.x) rv.x = cur_rv;
			cur_data = next_line+1;
			next_line = wcschr(cur_data, L'\n');
		}
		const auto	sz_left = wcslen(cur_data);
		const auto	last_rv = fn_print_row(cur_data, cur_data + sz_left);
		if(last_rv > rv.x) rv.x = last_rv;
		rv.y = (size_t)ImGui::GetCursorPosY();

		// pop global alpha value
		ImGui::PopStyleVar();
		return rv;
	}
}

static void compute_swapchain_display(struct swapchain_data *data)
{
   ImGui::SetCurrentContext(data->imgui_context);
   ImGui::NewFrame();
   // setup basic font and spacing
   ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
   if(vkdto::opt::font_x_size <= 0.0)
	   vkdto::set_font_x_size(data->ubuntu_mon_reg, data->ubuntu_mon_bold);
   ImGui::PushFont(data->ubuntu_mon_reg);
   vkdto::position_layer(data);
   ImGui::Begin("vkdto", 0, ImGuiWindowFlags_NoDecoration);

   const wchar_t	*cur_data = vkdto::sample_data();
   const auto		rc = vkdto::draw_data(cur_data, data);

   data->window_size = ImVec2(rc.x + 8.0, rc.y + 8.0);
   ImGui::End();
   ImGui::PopFont();
   ImGui::PopStyleVar();
   ImGui::EndFrame();
   ImGui::Render();
}

static uint32_t vk_memory_type(struct device_data *data,
                               VkMemoryPropertyFlags properties,
                               uint32_t type_bits)
{
    VkPhysicalDeviceMemoryProperties prop;
    data->instance->vtable.GetPhysicalDeviceMemoryProperties(data->physical_device, &prop);
    for (uint32_t i = 0; i < prop.memoryTypeCount; i++)
        if ((prop.memoryTypes[i].propertyFlags & properties) == properties && type_bits & (1<<i))
            return i;
    return 0xFFFFFFFF; // Unable to find memoryType
}

static void ensure_swapchain_fonts(struct swapchain_data *data,
                                   VkCommandBuffer command_buffer)
{
   if (data->font_uploaded)
      return;

   data->font_uploaded = true;

   struct device_data *device_data = data->device;
   ImGuiIO& io = ImGui::GetIO();
   unsigned char* pixels;
   int width, height;
   io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
   size_t upload_size = width * height * 4 * sizeof(char);

   /* Upload buffer */
   VkBufferCreateInfo buffer_info = {};
   buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   buffer_info.size = upload_size;
   buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
   buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   VK_CHECK(device_data->vtable.CreateBuffer(device_data->device, &buffer_info,
                                             NULL, &data->upload_font_buffer));
   VkMemoryRequirements upload_buffer_req;
   device_data->vtable.GetBufferMemoryRequirements(device_data->device,
                                                   data->upload_font_buffer,
                                                   &upload_buffer_req);
   VkMemoryAllocateInfo upload_alloc_info = {};
   upload_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   upload_alloc_info.allocationSize = upload_buffer_req.size;
   upload_alloc_info.memoryTypeIndex = vk_memory_type(device_data,
                                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                      upload_buffer_req.memoryTypeBits);
   VK_CHECK(device_data->vtable.AllocateMemory(device_data->device,
                                               &upload_alloc_info,
                                               NULL,
                                               &data->upload_font_buffer_mem));
   VK_CHECK(device_data->vtable.BindBufferMemory(device_data->device,
                                                 data->upload_font_buffer,
                                                 data->upload_font_buffer_mem, 0));

   /* Upload to Buffer */
   char* map = NULL;
   VK_CHECK(device_data->vtable.MapMemory(device_data->device,
                                          data->upload_font_buffer_mem,
                                          0, upload_size, 0, (void**)(&map)));
   memcpy(map, pixels, upload_size);
   VkMappedMemoryRange range[1] = {};
   range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
   range[0].memory = data->upload_font_buffer_mem;
   range[0].size = upload_size;
   VK_CHECK(device_data->vtable.FlushMappedMemoryRanges(device_data->device, 1, range));
   device_data->vtable.UnmapMemory(device_data->device,
                                   data->upload_font_buffer_mem);

   /* Copy buffer to image */
   VkImageMemoryBarrier copy_barrier[1] = {};
   copy_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   copy_barrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   copy_barrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   copy_barrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   copy_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   copy_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   copy_barrier[0].image = data->font_image;
   copy_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   copy_barrier[0].subresourceRange.levelCount = 1;
   copy_barrier[0].subresourceRange.layerCount = 1;
   device_data->vtable.CmdPipelineBarrier(command_buffer,
                                          VK_PIPELINE_STAGE_HOST_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          0, 0, NULL, 0, NULL,
                                          1, copy_barrier);

   VkBufferImageCopy region = {};
   region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   region.imageSubresource.layerCount = 1;
   region.imageExtent.width = width;
   region.imageExtent.height = height;
   region.imageExtent.depth = 1;
   device_data->vtable.CmdCopyBufferToImage(command_buffer,
                                            data->upload_font_buffer,
                                            data->font_image,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            1, &region);

   VkImageMemoryBarrier use_barrier[1] = {};
   use_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   use_barrier[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   use_barrier[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
   use_barrier[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   use_barrier[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   use_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   use_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   use_barrier[0].image = data->font_image;
   use_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   use_barrier[0].subresourceRange.levelCount = 1;
   use_barrier[0].subresourceRange.layerCount = 1;
   device_data->vtable.CmdPipelineBarrier(command_buffer,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                          0,
                                          0, NULL,
                                          0, NULL,
                                          1, use_barrier);

   /* Store our identifier */
   io.Fonts->TexID = (ImTextureID)(intptr_t)data->font_image;
}

static void CreateOrResizeBuffer(struct device_data *data,
                                 VkBuffer *buffer,
                                 VkDeviceMemory *buffer_memory,
                                 VkDeviceSize *buffer_size,
                                 size_t new_size, VkBufferUsageFlagBits usage)
{
    if (*buffer != VK_NULL_HANDLE)
        data->vtable.DestroyBuffer(data->device, *buffer, NULL);
    if (*buffer_memory)
        data->vtable.FreeMemory(data->device, *buffer_memory, NULL);

    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = new_size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(data->vtable.CreateBuffer(data->device, &buffer_info, NULL, buffer));

    VkMemoryRequirements req;
    data->vtable.GetBufferMemoryRequirements(data->device, *buffer, &req);
    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = req.size;
    alloc_info.memoryTypeIndex =
       vk_memory_type(data, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, req.memoryTypeBits);
    VK_CHECK(data->vtable.AllocateMemory(data->device, &alloc_info, NULL, buffer_memory));

    VK_CHECK(data->vtable.BindBufferMemory(data->device, *buffer, *buffer_memory, 0));
    *buffer_size = new_size;
}

static struct overlay_draw *render_swapchain_display(struct swapchain_data *data,
                                                     struct queue_data *present_queue,
                                                     const VkSemaphore *wait_semaphores,
                                                     unsigned n_wait_semaphores,
                                                     unsigned image_index)
{
   ImDrawData* draw_data = ImGui::GetDrawData();
   if (draw_data->TotalVtxCount == 0)
      return NULL;

   struct device_data *device_data = data->device;
   struct overlay_draw *draw = get_overlay_draw(data);

   device_data->vtable.ResetCommandBuffer(draw->command_buffer, 0);

   VkRenderPassBeginInfo render_pass_info = {};
   render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
   render_pass_info.renderPass = data->render_pass;
   render_pass_info.framebuffer = data->framebuffers[image_index];
   render_pass_info.renderArea.extent.width = data->width;
   render_pass_info.renderArea.extent.height = data->height;

   VkCommandBufferBeginInfo buffer_begin_info = {};
   buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

   device_data->vtable.BeginCommandBuffer(draw->command_buffer, &buffer_begin_info);

   ensure_swapchain_fonts(data, draw->command_buffer);

   /* Bounce the image to display back to color attachment layout for
    * rendering on top of it.
    */
   VkImageMemoryBarrier imb;
   imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   imb.pNext = nullptr;
   imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   imb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   imb.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   imb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   imb.image = data->images[image_index];
   imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   imb.subresourceRange.baseMipLevel = 0;
   imb.subresourceRange.levelCount = 1;
   imb.subresourceRange.baseArrayLayer = 0;
   imb.subresourceRange.layerCount = 1;
   imb.srcQueueFamilyIndex = present_queue->family_index;
   imb.dstQueueFamilyIndex = device_data->graphic_queue->family_index;
   device_data->vtable.CmdPipelineBarrier(draw->command_buffer,
                                          VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                          VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                          0,          /* dependency flags */
                                          0, nullptr, /* memory barriers */
                                          0, nullptr, /* buffer memory barriers */
                                          1, &imb);   /* image memory barriers */

   device_data->vtable.CmdBeginRenderPass(draw->command_buffer, &render_pass_info,
                                          VK_SUBPASS_CONTENTS_INLINE);

   /* Create/Resize vertex & index buffers */
   size_t vertex_size = draw_data->TotalVtxCount * sizeof(ImDrawVert);
   size_t index_size = draw_data->TotalIdxCount * sizeof(ImDrawIdx);
   if (draw->vertex_buffer_size < vertex_size) {
      CreateOrResizeBuffer(device_data,
                           &draw->vertex_buffer,
                           &draw->vertex_buffer_mem,
                           &draw->vertex_buffer_size,
                           vertex_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
   }
   if (draw->index_buffer_size < index_size) {
      CreateOrResizeBuffer(device_data,
                           &draw->index_buffer,
                           &draw->index_buffer_mem,
                           &draw->index_buffer_size,
                           index_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
   }

    /* Upload vertex & index data */
    ImDrawVert* vtx_dst = NULL;
    ImDrawIdx* idx_dst = NULL;
    VK_CHECK(device_data->vtable.MapMemory(device_data->device, draw->vertex_buffer_mem,
                                           0, vertex_size, 0, (void**)(&vtx_dst)));
    VK_CHECK(device_data->vtable.MapMemory(device_data->device, draw->index_buffer_mem,
                                           0, index_size, 0, (void**)(&idx_dst)));
    for (int n = 0; n < draw_data->CmdListsCount; n++)
        {
           const ImDrawList* cmd_list = draw_data->CmdLists[n];
           memcpy(vtx_dst, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
           memcpy(idx_dst, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));
           vtx_dst += cmd_list->VtxBuffer.Size;
           idx_dst += cmd_list->IdxBuffer.Size;
        }
    VkMappedMemoryRange range[2] = {};
    range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range[0].memory = draw->vertex_buffer_mem;
    range[0].size = VK_WHOLE_SIZE;
    range[1].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range[1].memory = draw->index_buffer_mem;
    range[1].size = VK_WHOLE_SIZE;
    VK_CHECK(device_data->vtable.FlushMappedMemoryRanges(device_data->device, 2, range));
    device_data->vtable.UnmapMemory(device_data->device, draw->vertex_buffer_mem);
    device_data->vtable.UnmapMemory(device_data->device, draw->index_buffer_mem);

    /* Bind pipeline and descriptor sets */
    device_data->vtable.CmdBindPipeline(draw->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, data->pipeline);
    VkDescriptorSet desc_set[1] = { data->descriptor_set };
    device_data->vtable.CmdBindDescriptorSets(draw->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                              data->pipeline_layout, 0, 1, desc_set, 0, NULL);

    /* Bind vertex & index buffers */
    VkBuffer vertex_buffers[1] = { draw->vertex_buffer };
    VkDeviceSize vertex_offset[1] = { 0 };
    device_data->vtable.CmdBindVertexBuffers(draw->command_buffer, 0, 1, vertex_buffers, vertex_offset);
    device_data->vtable.CmdBindIndexBuffer(draw->command_buffer, draw->index_buffer, 0, VK_INDEX_TYPE_UINT16);

    /* Setup viewport */
    VkViewport viewport;
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = draw_data->DisplaySize.x;
    viewport.height = draw_data->DisplaySize.y;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    device_data->vtable.CmdSetViewport(draw->command_buffer, 0, 1, &viewport);


    /* Setup scale and translation through push constants :
     *
     * Our visible imgui space lies from draw_data->DisplayPos (top left) to
     * draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayMin
     * is typically (0,0) for single viewport apps.
     */
    float scale[2];
    scale[0] = 2.0f / draw_data->DisplaySize.x;
    scale[1] = 2.0f / draw_data->DisplaySize.y;
    float translate[2];
    translate[0] = -1.0f - draw_data->DisplayPos.x * scale[0];
    translate[1] = -1.0f - draw_data->DisplayPos.y * scale[1];
    device_data->vtable.CmdPushConstants(draw->command_buffer, data->pipeline_layout,
                                         VK_SHADER_STAGE_VERTEX_BIT,
                                         sizeof(float) * 0, sizeof(float) * 2, scale);
    device_data->vtable.CmdPushConstants(draw->command_buffer, data->pipeline_layout,
                                         VK_SHADER_STAGE_VERTEX_BIT,
                                         sizeof(float) * 2, sizeof(float) * 2, translate);

    // Render the command lists:
    int vtx_offset = 0;
    int idx_offset = 0;
    ImVec2 display_pos = draw_data->DisplayPos;
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
            // Apply scissor/clipping rectangle
            // FIXME: We could clamp width/height based on clamped min/max values.
            VkRect2D scissor;
            scissor.offset.x = (int32_t)(pcmd->ClipRect.x - display_pos.x) > 0 ? (int32_t)(pcmd->ClipRect.x - display_pos.x) : 0;
            scissor.offset.y = (int32_t)(pcmd->ClipRect.y - display_pos.y) > 0 ? (int32_t)(pcmd->ClipRect.y - display_pos.y) : 0;
            scissor.extent.width = (uint32_t)(pcmd->ClipRect.z - pcmd->ClipRect.x);
            scissor.extent.height = (uint32_t)(pcmd->ClipRect.w - pcmd->ClipRect.y + 1); // FIXME: Why +1 here?
            device_data->vtable.CmdSetScissor(draw->command_buffer, 0, 1, &scissor);

            // Draw
            device_data->vtable.CmdDrawIndexed(draw->command_buffer, pcmd->ElemCount, 1, idx_offset, vtx_offset, 0);

            idx_offset += pcmd->ElemCount;
        }
        vtx_offset += cmd_list->VtxBuffer.Size;
    }

   device_data->vtable.CmdEndRenderPass(draw->command_buffer);

   if (device_data->graphic_queue->family_index != present_queue->family_index)
   {
      /* Transfer the image back to the present queue family
       * image layout was already changed to present by the render pass 
       */
      imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      imb.pNext = nullptr;
      imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      imb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      imb.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      imb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      imb.image = data->images[image_index];
      imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      imb.subresourceRange.baseMipLevel = 0;
      imb.subresourceRange.levelCount = 1;
      imb.subresourceRange.baseArrayLayer = 0;
      imb.subresourceRange.layerCount = 1;
      imb.srcQueueFamilyIndex = device_data->graphic_queue->family_index;
      imb.dstQueueFamilyIndex = present_queue->family_index;
      device_data->vtable.CmdPipelineBarrier(draw->command_buffer,
                                             VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                             VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                             0,          /* dependency flags */
                                             0, nullptr, /* memory barriers */
                                             0, nullptr, /* buffer memory barriers */
                                             1, &imb);   /* image memory barriers */
   }

   device_data->vtable.EndCommandBuffer(draw->command_buffer);

   /* When presenting on a different queue than where we're drawing the
    * overlay *AND* when the application does not provide a semaphore to
    * vkQueuePresent, insert our own cross engine synchronization
    * semaphore.
    */
   if (n_wait_semaphores == 0 && device_data->graphic_queue->queue != present_queue->queue) {
      VkPipelineStageFlags stages_wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkSubmitInfo submit_info = {};
      submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit_info.commandBufferCount = 0;
      submit_info.pWaitDstStageMask = &stages_wait;
      submit_info.waitSemaphoreCount = 0;
      submit_info.signalSemaphoreCount = 1;
      submit_info.pSignalSemaphores = &draw->cross_engine_semaphore;

      device_data->vtable.QueueSubmit(present_queue->queue, 1, &submit_info, VK_NULL_HANDLE);

      submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit_info.commandBufferCount = 1;
      submit_info.pWaitDstStageMask = &stages_wait;
      submit_info.pCommandBuffers = &draw->command_buffer;
      submit_info.waitSemaphoreCount = 1;
      submit_info.pWaitSemaphores = &draw->cross_engine_semaphore;
      submit_info.signalSemaphoreCount = 1;
      submit_info.pSignalSemaphores = &draw->semaphore;

      device_data->vtable.QueueSubmit(device_data->graphic_queue->queue, 1, &submit_info, draw->fence);
   } else {
      VkPipelineStageFlags *stages_wait = (VkPipelineStageFlags*) malloc(sizeof(VkPipelineStageFlags) * n_wait_semaphores);
      for (unsigned i = 0; i < n_wait_semaphores; i++)
      {
         // wait in the fragment stage until the swapchain image is ready
         stages_wait[i] = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      }

      VkSubmitInfo submit_info = {};
      submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &draw->command_buffer;
      submit_info.pWaitDstStageMask = stages_wait;
      submit_info.waitSemaphoreCount = n_wait_semaphores;
      submit_info.pWaitSemaphores = wait_semaphores;
      submit_info.signalSemaphoreCount = 1;
      submit_info.pSignalSemaphores = &draw->semaphore;

      device_data->vtable.QueueSubmit(device_data->graphic_queue->queue, 1, &submit_info, draw->fence);

      free(stages_wait);
   }

   return draw;
}

static const uint32_t overlay_vert_spv[] = {
#include "overlay.vert.spv.h"
};
static const uint32_t overlay_frag_spv[] = {
#include "overlay.frag.spv.h"
};

static void setup_swapchain_data_pipeline(struct swapchain_data *data)
{
   struct device_data *device_data = data->device;
   VkShaderModule vert_module, frag_module;

   /* Create shader modules */
   VkShaderModuleCreateInfo vert_info = {};
   vert_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
   vert_info.codeSize = sizeof(overlay_vert_spv);
   vert_info.pCode = overlay_vert_spv;
   VK_CHECK(device_data->vtable.CreateShaderModule(device_data->device,
                                                   &vert_info, NULL, &vert_module));
   VkShaderModuleCreateInfo frag_info = {};
   frag_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
   frag_info.codeSize = sizeof(overlay_frag_spv);
   frag_info.pCode = (uint32_t*)overlay_frag_spv;
   VK_CHECK(device_data->vtable.CreateShaderModule(device_data->device,
                                                   &frag_info, NULL, &frag_module));

   /* Font sampler */
   VkSamplerCreateInfo sampler_info = {};
   sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
   sampler_info.magFilter = VK_FILTER_LINEAR;
   sampler_info.minFilter = VK_FILTER_LINEAR;
   sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
   sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
   sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
   sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
   sampler_info.minLod = -1000;
   sampler_info.maxLod = 1000;
   sampler_info.maxAnisotropy = 1.0f;
   VK_CHECK(device_data->vtable.CreateSampler(device_data->device, &sampler_info,
                                              NULL, &data->font_sampler));

   /* Descriptor pool */
   VkDescriptorPoolSize sampler_pool_size = {};
   sampler_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   sampler_pool_size.descriptorCount = 1;
   VkDescriptorPoolCreateInfo desc_pool_info = {};
   desc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   desc_pool_info.maxSets = 1;
   desc_pool_info.poolSizeCount = 1;
   desc_pool_info.pPoolSizes = &sampler_pool_size;
   VK_CHECK(device_data->vtable.CreateDescriptorPool(device_data->device,
                                                     &desc_pool_info,
                                                     NULL, &data->descriptor_pool));

   /* Descriptor layout */
   VkSampler sampler[1] = { data->font_sampler };
   VkDescriptorSetLayoutBinding binding[1] = {};
   binding[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   binding[0].descriptorCount = 1;
   binding[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
   binding[0].pImmutableSamplers = sampler;
   VkDescriptorSetLayoutCreateInfo set_layout_info = {};
   set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
   set_layout_info.bindingCount = 1;
   set_layout_info.pBindings = binding;
   VK_CHECK(device_data->vtable.CreateDescriptorSetLayout(device_data->device,
                                                          &set_layout_info,
                                                          NULL, &data->descriptor_layout));

   /* Descriptor set */
   VkDescriptorSetAllocateInfo alloc_info = {};
   alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
   alloc_info.descriptorPool = data->descriptor_pool;
   alloc_info.descriptorSetCount = 1;
   alloc_info.pSetLayouts = &data->descriptor_layout;
   VK_CHECK(device_data->vtable.AllocateDescriptorSets(device_data->device,
                                                       &alloc_info,
                                                       &data->descriptor_set));

   /* Constants: we are using 'vec2 offset' and 'vec2 scale' instead of a full
    * 3d projection matrix
    */
   VkPushConstantRange push_constants[1] = {};
   push_constants[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
   push_constants[0].offset = sizeof(float) * 0;
   push_constants[0].size = sizeof(float) * 4;
   VkPipelineLayoutCreateInfo layout_info = {};
   layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
   layout_info.setLayoutCount = 1;
   layout_info.pSetLayouts = &data->descriptor_layout;
   layout_info.pushConstantRangeCount = 1;
   layout_info.pPushConstantRanges = push_constants;
   VK_CHECK(device_data->vtable.CreatePipelineLayout(device_data->device,
                                                     &layout_info,
                                                     NULL, &data->pipeline_layout));

   VkPipelineShaderStageCreateInfo stage[2] = {};
   stage[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   stage[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
   stage[0].module = vert_module;
   stage[0].pName = "main";
   stage[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   stage[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
   stage[1].module = frag_module;
   stage[1].pName = "main";

   VkVertexInputBindingDescription binding_desc[1] = {};
   binding_desc[0].stride = sizeof(ImDrawVert);
   binding_desc[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

   VkVertexInputAttributeDescription attribute_desc[3] = {};
   attribute_desc[0].location = 0;
   attribute_desc[0].binding = binding_desc[0].binding;
   attribute_desc[0].format = VK_FORMAT_R32G32_SFLOAT;
   attribute_desc[0].offset = IM_OFFSETOF(ImDrawVert, pos);
   attribute_desc[1].location = 1;
   attribute_desc[1].binding = binding_desc[0].binding;
   attribute_desc[1].format = VK_FORMAT_R32G32_SFLOAT;
   attribute_desc[1].offset = IM_OFFSETOF(ImDrawVert, uv);
   attribute_desc[2].location = 2;
   attribute_desc[2].binding = binding_desc[0].binding;
   attribute_desc[2].format = VK_FORMAT_R8G8B8A8_UNORM;
   attribute_desc[2].offset = IM_OFFSETOF(ImDrawVert, col);

   VkPipelineVertexInputStateCreateInfo vertex_info = {};
   vertex_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
   vertex_info.vertexBindingDescriptionCount = 1;
   vertex_info.pVertexBindingDescriptions = binding_desc;
   vertex_info.vertexAttributeDescriptionCount = 3;
   vertex_info.pVertexAttributeDescriptions = attribute_desc;

   VkPipelineInputAssemblyStateCreateInfo ia_info = {};
   ia_info.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
   ia_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

   VkPipelineViewportStateCreateInfo viewport_info = {};
   viewport_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
   viewport_info.viewportCount = 1;
   viewport_info.scissorCount = 1;

   VkPipelineRasterizationStateCreateInfo raster_info = {};
   raster_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
   raster_info.polygonMode = VK_POLYGON_MODE_FILL;
   raster_info.cullMode = VK_CULL_MODE_NONE;
   raster_info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
   raster_info.lineWidth = 1.0f;

   VkPipelineMultisampleStateCreateInfo ms_info = {};
   ms_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
   ms_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

   VkPipelineColorBlendAttachmentState color_attachment[1] = {};
   color_attachment[0].blendEnable = VK_TRUE;
   color_attachment[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
   color_attachment[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
   color_attachment[0].colorBlendOp = VK_BLEND_OP_ADD;
   color_attachment[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
   color_attachment[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
   color_attachment[0].alphaBlendOp = VK_BLEND_OP_ADD;
   color_attachment[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
      VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

   VkPipelineDepthStencilStateCreateInfo depth_info = {};
   depth_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

   VkPipelineColorBlendStateCreateInfo blend_info = {};
   blend_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
   blend_info.attachmentCount = 1;
   blend_info.pAttachments = color_attachment;

   VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
   VkPipelineDynamicStateCreateInfo dynamic_state = {};
   dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
   dynamic_state.dynamicStateCount = (uint32_t)IM_ARRAYSIZE(dynamic_states);
   dynamic_state.pDynamicStates = dynamic_states;

   VkGraphicsPipelineCreateInfo info = {};
   info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
   info.flags = 0;
   info.stageCount = 2;
   info.pStages = stage;
   info.pVertexInputState = &vertex_info;
   info.pInputAssemblyState = &ia_info;
   info.pViewportState = &viewport_info;
   info.pRasterizationState = &raster_info;
   info.pMultisampleState = &ms_info;
   info.pDepthStencilState = &depth_info;
   info.pColorBlendState = &blend_info;
   info.pDynamicState = &dynamic_state;
   info.layout = data->pipeline_layout;
   info.renderPass = data->render_pass;
   VK_CHECK(
      device_data->vtable.CreateGraphicsPipelines(device_data->device, VK_NULL_HANDLE,
                                                  1, &info,
                                                  NULL, &data->pipeline));

   device_data->vtable.DestroyShaderModule(device_data->device, vert_module, NULL);
   device_data->vtable.DestroyShaderModule(device_data->device, frag_module, NULL);

   ImGuiIO& io = ImGui::GetIO();
   unsigned char* pixels;
   int width, height;
   // load fonts here
   const static ImWchar	ALL_RANGES[] = {
   	0x0020, 0xFFFF,
	0
   };
   // setup the default font in case ubuntu ones can't be found
   // nor loaded
   ImFont* default_font = 0;
   {
	ImFontConfig	cfg;
	cfg.SizePixels = vkdto::opt::font_size;
	default_font = io.Fonts->AddFontDefault(&cfg);
	assert(default_font);
   }
   if(!data->ubuntu_mon_reg) {
	const static char	font_path[] = "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf";
	struct stat		s = {0};
	if(!stat(font_path, &s))
   		data->ubuntu_mon_reg = io.Fonts->AddFontFromFileTTF(font_path, vkdto::opt::font_size, 0, ALL_RANGES);
	if(!data->ubuntu_mon_reg)
		data->ubuntu_mon_reg = default_font;
	assert(data->ubuntu_mon_reg);
   }
   if(!data->ubuntu_mon_bold) {
	const static char	font_path[] = "/usr/share/fonts/truetype/ubuntu/UbuntuMono-B.ttf";
	struct stat		s = {0};
	if(!stat(font_path, &s))
   		data->ubuntu_mon_bold = io.Fonts->AddFontFromFileTTF(font_path, vkdto::opt::font_size, 0, ALL_RANGES);
	if(!data->ubuntu_mon_bold)
		data->ubuntu_mon_bold = default_font;
	assert(data->ubuntu_mon_bold);
   }
   io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

   /* Font image */
   VkImageCreateInfo image_info = {};
   image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   image_info.imageType = VK_IMAGE_TYPE_2D;
   image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
   image_info.extent.width = width;
   image_info.extent.height = height;
   image_info.extent.depth = 1;
   image_info.mipLevels = 1;
   image_info.arrayLayers = 1;
   image_info.samples = VK_SAMPLE_COUNT_1_BIT;
   image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
   image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   VK_CHECK(device_data->vtable.CreateImage(device_data->device, &image_info,
                                            NULL, &data->font_image));
   VkMemoryRequirements font_image_req;
   device_data->vtable.GetImageMemoryRequirements(device_data->device,
                                                  data->font_image, &font_image_req);
   VkMemoryAllocateInfo image_alloc_info = {};
   image_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   image_alloc_info.allocationSize = font_image_req.size;
   image_alloc_info.memoryTypeIndex = vk_memory_type(device_data,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                     font_image_req.memoryTypeBits);
   VK_CHECK(device_data->vtable.AllocateMemory(device_data->device, &image_alloc_info,
                                               NULL, &data->font_mem));
   VK_CHECK(device_data->vtable.BindImageMemory(device_data->device,
                                                data->font_image,
                                                data->font_mem, 0));

   /* Font image view */
   VkImageViewCreateInfo view_info = {};
   view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   view_info.image = data->font_image;
   view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
   view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
   view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   view_info.subresourceRange.levelCount = 1;
   view_info.subresourceRange.layerCount = 1;
   VK_CHECK(device_data->vtable.CreateImageView(device_data->device, &view_info,
                                                NULL, &data->font_image_view));

   /* Descriptor set */
   VkDescriptorImageInfo desc_image[1] = {};
   desc_image[0].sampler = data->font_sampler;
   desc_image[0].imageView = data->font_image_view;
   desc_image[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   VkWriteDescriptorSet write_desc[1] = {};
   write_desc[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
   write_desc[0].dstSet = data->descriptor_set;
   write_desc[0].descriptorCount = 1;
   write_desc[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   write_desc[0].pImageInfo = desc_image;
   device_data->vtable.UpdateDescriptorSets(device_data->device, 1, write_desc, 0, NULL);
}

static void setup_swapchain_data(struct swapchain_data *data,
                                 const VkSwapchainCreateInfoKHR *pCreateInfo)
{
   // load options here, before we instantiate
   // ImGui objects and whatnot
   vkdto::load_opt();

   if(vkdto::debug::log) {
	   std::fprintf(vkdto::debug::log,
			   "%s %s swapchain_data %p pCreateInfo->imageExtent.width %d, pCreateInfo->imageExtent.height %d\n",
			   vkdto::debug::get_ts().c_str(),
			   __FUNCTION__,
			   data,
			   pCreateInfo->imageExtent.width,
			   pCreateInfo->imageExtent.height);
	   std::fflush(vkdto::debug::log);
   }

   data->width = pCreateInfo->imageExtent.width;
   data->height = pCreateInfo->imageExtent.height;
   data->format = pCreateInfo->imageFormat;

   data->imgui_context = ImGui::CreateContext();
   ImGui::SetCurrentContext(data->imgui_context);

   ImGui::GetIO().IniFilename = NULL;
   ImGui::GetIO().DisplaySize = ImVec2((float)data->width, (float)data->height);

   struct device_data *device_data = data->device;

   /* Render pass */
   VkAttachmentDescription attachment_desc = {};
   attachment_desc.format = pCreateInfo->imageFormat;
   attachment_desc.samples = VK_SAMPLE_COUNT_1_BIT;
   attachment_desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
   attachment_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   attachment_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   attachment_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   attachment_desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   attachment_desc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   VkAttachmentReference color_attachment = {};
   color_attachment.attachment = 0;
   color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   VkSubpassDescription subpass = {};
   subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   subpass.colorAttachmentCount = 1;
   subpass.pColorAttachments = &color_attachment;
   VkSubpassDependency dependency = {};
   dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
   dependency.dstSubpass = 0;
   dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   dependency.srcAccessMask = 0;
   dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   VkRenderPassCreateInfo render_pass_info = {};
   render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
   render_pass_info.attachmentCount = 1;
   render_pass_info.pAttachments = &attachment_desc;
   render_pass_info.subpassCount = 1;
   render_pass_info.pSubpasses = &subpass;
   render_pass_info.dependencyCount = 1;
   render_pass_info.pDependencies = &dependency;
   VK_CHECK(device_data->vtable.CreateRenderPass(device_data->device,
                                                 &render_pass_info,
                                                 NULL, &data->render_pass));

   setup_swapchain_data_pipeline(data);

   VK_CHECK(device_data->vtable.GetSwapchainImagesKHR(device_data->device,
                                                      data->swapchain,
                                                      &data->n_images,
                                                      NULL));

   data->images = ralloc_array(data, VkImage, data->n_images);
   data->image_views = ralloc_array(data, VkImageView, data->n_images);
   data->framebuffers = ralloc_array(data, VkFramebuffer, data->n_images);

   VK_CHECK(device_data->vtable.GetSwapchainImagesKHR(device_data->device,
                                                      data->swapchain,
                                                      &data->n_images,
                                                      data->images));

   /* Image views */
   VkImageViewCreateInfo view_info = {};
   view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
   view_info.format = pCreateInfo->imageFormat;
   view_info.components.r = VK_COMPONENT_SWIZZLE_R;
   view_info.components.g = VK_COMPONENT_SWIZZLE_G;
   view_info.components.b = VK_COMPONENT_SWIZZLE_B;
   view_info.components.a = VK_COMPONENT_SWIZZLE_A;
   view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   for (uint32_t i = 0; i < data->n_images; i++) {
      view_info.image = data->images[i];
      VK_CHECK(device_data->vtable.CreateImageView(device_data->device,
                                                   &view_info, NULL,
                                                   &data->image_views[i]));
   }

   /* Framebuffers */
   VkImageView attachment[1];
   VkFramebufferCreateInfo fb_info = {};
   fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
   fb_info.renderPass = data->render_pass;
   fb_info.attachmentCount = 1;
   fb_info.pAttachments = attachment;
   fb_info.width = data->width;
   fb_info.height = data->height;
   fb_info.layers = 1;
   for (uint32_t i = 0; i < data->n_images; i++) {
      attachment[0] = data->image_views[i];
      VK_CHECK(device_data->vtable.CreateFramebuffer(device_data->device, &fb_info,
                                                     NULL, &data->framebuffers[i]));
   }

   /* Command buffer pool */
   VkCommandPoolCreateInfo cmd_buffer_pool_info = {};
   cmd_buffer_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   cmd_buffer_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   cmd_buffer_pool_info.queueFamilyIndex = device_data->graphic_queue->family_index;
   VK_CHECK(device_data->vtable.CreateCommandPool(device_data->device,
                                                  &cmd_buffer_pool_info,
                                                  NULL, &data->command_pool));
}

static void shutdown_swapchain_data(struct swapchain_data *data)
{
   if(vkdto::debug::log) {
	   std::fprintf(vkdto::debug::log,
			   "%s %s swapchain_data %p\n",
			   vkdto::debug::get_ts().c_str(),
			   __FUNCTION__,
			   data);
	   std::fflush(vkdto::debug::log);
   }

   struct device_data *device_data = data->device;

   list_for_each_entry_safe(struct overlay_draw, draw, &data->draws, link) {
      device_data->vtable.DestroySemaphore(device_data->device, draw->cross_engine_semaphore, NULL);
      device_data->vtable.DestroySemaphore(device_data->device, draw->semaphore, NULL);
      device_data->vtable.DestroyFence(device_data->device, draw->fence, NULL);
      device_data->vtable.DestroyBuffer(device_data->device, draw->vertex_buffer, NULL);
      device_data->vtable.DestroyBuffer(device_data->device, draw->index_buffer, NULL);
      device_data->vtable.FreeMemory(device_data->device, draw->vertex_buffer_mem, NULL);
      device_data->vtable.FreeMemory(device_data->device, draw->index_buffer_mem, NULL);
   }

   for (uint32_t i = 0; i < data->n_images; i++) {
      device_data->vtable.DestroyImageView(device_data->device, data->image_views[i], NULL);
      device_data->vtable.DestroyFramebuffer(device_data->device, data->framebuffers[i], NULL);
   }

   device_data->vtable.DestroyRenderPass(device_data->device, data->render_pass, NULL);

   device_data->vtable.DestroyCommandPool(device_data->device, data->command_pool, NULL);

   device_data->vtable.DestroyPipeline(device_data->device, data->pipeline, NULL);
   device_data->vtable.DestroyPipelineLayout(device_data->device, data->pipeline_layout, NULL);

   device_data->vtable.DestroyDescriptorPool(device_data->device,
                                             data->descriptor_pool, NULL);
   device_data->vtable.DestroyDescriptorSetLayout(device_data->device,
                                                  data->descriptor_layout, NULL);

   device_data->vtable.DestroySampler(device_data->device, data->font_sampler, NULL);
   device_data->vtable.DestroyImageView(device_data->device, data->font_image_view, NULL);
   device_data->vtable.DestroyImage(device_data->device, data->font_image, NULL);
   device_data->vtable.FreeMemory(device_data->device, data->font_mem, NULL);

   device_data->vtable.DestroyBuffer(device_data->device, data->upload_font_buffer, NULL);
   device_data->vtable.FreeMemory(device_data->device, data->upload_font_buffer_mem, NULL);

   ImGui::DestroyContext(data->imgui_context);
}

static struct overlay_draw *before_present(struct swapchain_data *swapchain_data,
                                           struct queue_data *present_queue,
                                           const VkSemaphore *wait_semaphores,
                                           unsigned n_wait_semaphores,
                                           unsigned imageIndex)
{
   struct overlay_draw *draw = NULL;

   compute_swapchain_display(swapchain_data);
   draw = render_swapchain_display(swapchain_data, present_queue,
                                   wait_semaphores, n_wait_semaphores,
                                   imageIndex);

   if(vkdto::debug::log) {
      std::fprintf(vkdto::debug::log,
		      "%s %s swapchain_data %p present_queue %p overlay_draw %p\n",
		      vkdto::debug::get_ts().c_str(),
		      __FUNCTION__,
		      swapchain_data,
		      present_queue,
		      draw);
      std::fflush(vkdto::debug::log);
   }

   return draw;
}

static VkResult overlay_CreateSwapchainKHR(
    VkDevice                                    device,
    const VkSwapchainCreateInfoKHR*             pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSwapchainKHR*                             pSwapchain)
{
   struct device_data *device_data = FIND(struct device_data, device);
   VkResult result = device_data->vtable.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
   if (result != VK_SUCCESS) return result;

   struct swapchain_data *swapchain_data = new_swapchain_data(*pSwapchain, device_data);
   setup_swapchain_data(swapchain_data, pCreateInfo);
   return result;
}

static void overlay_DestroySwapchainKHR(
    VkDevice                                    device,
    VkSwapchainKHR                              swapchain,
    const VkAllocationCallbacks*                pAllocator)
{
   struct swapchain_data *swapchain_data =
      FIND(struct swapchain_data, swapchain);

   shutdown_swapchain_data(swapchain_data);
   swapchain_data->device->vtable.DestroySwapchainKHR(device, swapchain, pAllocator);
   destroy_swapchain_data(swapchain_data);
}

static VkResult overlay_QueuePresentKHR(
    VkQueue                                     queue,
    const VkPresentInfoKHR*                     pPresentInfo)
{
   struct queue_data *queue_data = FIND(struct queue_data, queue);

   /* Otherwise we need to add our overlay drawing semaphore to the list of
    * semaphores to wait on. If we don't do that the presented picture might
    * be have incomplete overlay drawings.
    */
   VkResult result = VK_SUCCESS;
   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
      VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[i];
      struct swapchain_data *swapchain_data =
         FIND(struct swapchain_data, swapchain);

      uint32_t image_index = pPresentInfo->pImageIndices[i];

      VkPresentInfoKHR present_info = *pPresentInfo;
      present_info.swapchainCount = 1;
      present_info.pSwapchains = &swapchain;
      present_info.pImageIndices = &image_index;

      struct overlay_draw *draw = before_present(swapchain_data,
                                                 queue_data,
                                                 pPresentInfo->pWaitSemaphores,
                                                 pPresentInfo->waitSemaphoreCount,
                                                 image_index);

      /* Because the submission of the overlay draw waits on the semaphores
       * handed for present, we don't need to have this present operation
       * wait on them as well, we can just wait on the overlay submission
       * semaphore.
       */
      present_info.pWaitSemaphores = &draw->semaphore;
      present_info.waitSemaphoreCount = 1;

      VkResult chain_result = queue_data->device->vtable.QueuePresentKHR(queue, &present_info);
      if (pPresentInfo->pResults)
         pPresentInfo->pResults[i] = chain_result;
      if (chain_result != VK_SUCCESS && result == VK_SUCCESS)
         result = chain_result;
   }
   return result;
}

static VkResult overlay_CreateDevice(
    VkPhysicalDevice                            physicalDevice,
    const VkDeviceCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDevice*                                   pDevice)
{
   struct instance_data *instance_data =
      FIND(struct instance_data, physicalDevice);
   VkLayerDeviceCreateInfo *chain_info =
      get_device_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

   assert(chain_info->u.pLayerInfo);
   PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
   PFN_vkCreateDevice fpCreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(NULL, "vkCreateDevice");
   if (fpCreateDevice == NULL) {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   // Advance the link info for the next element on the chain
   chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

   VkPhysicalDeviceFeatures device_features = {};
   VkDeviceCreateInfo device_info = *pCreateInfo;

   if (pCreateInfo->pEnabledFeatures)
      device_features = *(pCreateInfo->pEnabledFeatures);
   device_info.pEnabledFeatures = &device_features;


   VkResult result = fpCreateDevice(physicalDevice, &device_info, pAllocator, pDevice);
   if (result != VK_SUCCESS) return result;

   struct device_data *device_data = new_device_data(*pDevice, instance_data);
   device_data->physical_device = physicalDevice;
   vk_load_device_commands(*pDevice, fpGetDeviceProcAddr, &device_data->vtable);

   instance_data->vtable.GetPhysicalDeviceProperties(device_data->physical_device,
                                                     &device_data->properties);

   VkLayerDeviceCreateInfo *load_data_info =
      get_device_chain_info(pCreateInfo, VK_LOADER_DATA_CALLBACK);
   device_data->set_device_loader_data = load_data_info->u.pfnSetDeviceLoaderData;

   device_map_queues(device_data, pCreateInfo);

   return result;
}

static void overlay_DestroyDevice(
    VkDevice                                    device,
    const VkAllocationCallbacks*                pAllocator)
{
   struct device_data *device_data = FIND(struct device_data, device);
   device_unmap_queues(device_data);
   device_data->vtable.DestroyDevice(device, pAllocator);
   destroy_device_data(device_data);
}

static VkResult overlay_CreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkInstance*                                 pInstance)
{
   VkLayerInstanceCreateInfo *chain_info =
      get_instance_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

   assert(chain_info->u.pLayerInfo);
   PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr =
      chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   PFN_vkCreateInstance fpCreateInstance =
      (PFN_vkCreateInstance)fpGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (fpCreateInstance == NULL) {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   // Advance the link info for the next element on the chain
   chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

   VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
   if (result != VK_SUCCESS) return result;

   struct instance_data *instance_data = new_instance_data(*pInstance);
   vk_load_instance_commands(instance_data->instance,
                             fpGetInstanceProcAddr,
                             &instance_data->vtable);
   instance_data_map_physical_devices(instance_data, true);

   return result;
}

static void overlay_DestroyInstance(
    VkInstance                                  instance,
    const VkAllocationCallbacks*                pAllocator)
{
   struct instance_data *instance_data = FIND(struct instance_data, instance);
   instance_data_map_physical_devices(instance_data, false);
   instance_data->vtable.DestroyInstance(instance, pAllocator);
   destroy_instance_data(instance_data);
}

extern "C" VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkdto_vkGetDeviceProcAddr(VkDevice dev,
                                                                              const char *funcName);

static const struct {
   const char *name;
   void *ptr;
} name_to_funcptr_map[] = {
   { "vkGetDeviceProcAddr", (void *) vkdto_vkGetDeviceProcAddr },
#define ADD_HOOK(fn) { "vk" # fn, (void *) overlay_ ## fn }
#define ADD_ALIAS_HOOK(alias, fn) { "vk" # alias, (void *) overlay_ ## fn }
   ADD_HOOK(CreateSwapchainKHR),
   ADD_HOOK(QueuePresentKHR),
   ADD_HOOK(DestroySwapchainKHR),

   ADD_HOOK(CreateDevice),
   ADD_HOOK(DestroyDevice),

   ADD_HOOK(CreateInstance),
   ADD_HOOK(DestroyInstance),
#undef ADD_HOOK
};

static void *find_ptr(const char *name)
{
   for (uint32_t i = 0; i < ARRAY_SIZE(name_to_funcptr_map); i++) {
      if (strcmp(name, name_to_funcptr_map[i].name) == 0)
         return name_to_funcptr_map[i].ptr;
   }

   return NULL;
}

extern "C" VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkdto_vkGetDeviceProcAddr(VkDevice dev,
                                                                             const char *funcName)
{
   void *ptr = find_ptr(funcName);
   if (ptr) return reinterpret_cast<PFN_vkVoidFunction>(ptr);

   if (dev == NULL) return NULL;

   struct device_data *device_data = FIND(struct device_data, dev);
   if (device_data->vtable.GetDeviceProcAddr == NULL) return NULL;
   return device_data->vtable.GetDeviceProcAddr(dev, funcName);
}

extern "C" VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkdto_vkGetInstanceProcAddr(VkInstance instance,
                                                                               const char *funcName)
{
   void *ptr = find_ptr(funcName);
   if (ptr) return reinterpret_cast<PFN_vkVoidFunction>(ptr);

   if (instance == NULL) return NULL;

   struct instance_data *instance_data = FIND(struct instance_data, instance);
   if (instance_data->vtable.GetInstanceProcAddr == NULL) return NULL;
   return instance_data->vtable.GetInstanceProcAddr(instance, funcName);
}

