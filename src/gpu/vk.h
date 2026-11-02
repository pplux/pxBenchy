#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <volk.h>
#include <vk_mem_alloc.h> // after volk.h: enables vmaImportVulkanFunctionsFromVolk
#include <tracy/TracyVulkan.hpp>

namespace vkc {

// What a resource is used for, one bit per use. Each mode translates uses to its own stage/access/layout (OR of the
// rows for the set bits). Image uses name their Layout; the rest are buffer-only.
enum Use : uint32_t {
  USE_NONE = 0,                    // UNDEFINED: never used, contents discarded
  COPY_SRC = 1u << 0,              // TRANSFER_SRC
  COPY_DST = 1u << 1,              // TRANSFER_DST (write)
  BLIT_SRC = 1u << 2,              // TRANSFER_SRC
  BLIT_DST = 1u << 3,              // TRANSFER_DST (write)
  VERTEX_BUFFER = 1u << 4,         // buffer
  INDEX_BUFFER = 1u << 5,          // buffer
  INDIRECT_BUFFER = 1u << 6,       // buffer
  UNIFORM_GRAPHICS = 1u << 7,      // buffer, vertex + fragment shaders
  STORAGE_READ_GRAPHICS = 1u << 8, // buffer, vertex + fragment shaders
  STORAGE_RW_COMPUTE = 1u << 9,    // GENERAL (write)
  SAMPLED_FRAGMENT = 1u << 10,     // SHADER_READ_ONLY
  SAMPLED_COMPUTE = 1u << 11,      // SHADER_READ_ONLY
  COLOR_ATTACH_READ = 1u << 12,    // COLOR_ATTACHMENT (LOAD)
  COLOR_ATTACH_WRITE = 1u << 13,   // COLOR_ATTACHMENT (write)
  DEPTH_ATTACH = 1u << 14,         // DEPTH (write), read + write
  PRESENT = 1u << 15,              // PRESENT_SRC
  HOST_READ = 1u << 16,            // buffer
  ACQUIRE_COLOR = 1u << 17,        // UNDEFINED: swapchain image just acquired, semaphore waited at color output
  ACQUIRE_COPY = 1u << 18,         // UNDEFINED: swapchain image just acquired, semaphore waited at copy
};
constexpr uint32_t USE_COUNT = 19; // rows in each mode's use table
static_assert(ACQUIRE_COPY == 1u << (USE_COUNT - 1));

constexpr uint32_t USE_WRITES = COPY_DST | BLIT_DST | STORAGE_RW_COMPUTE | COLOR_ATTACH_WRITE | DEPTH_ATTACH;

// Last known (or wanted) uses of a resource.
struct State {
  uint32_t uses = USE_NONE;
};

// Mode-neutral image layout. DEPTH is DEPTH_ATTACHMENT_OPTIMAL in vk13, DEPTH_STENCIL_ATTACHMENT_OPTIMAL in vk11.
enum class Layout {
  UNDEFINED,
  GENERAL,
  COLOR_ATTACHMENT,
  DEPTH,
  SHADER_READ_ONLY,
  TRANSFER_SRC,
  TRANSFER_DST,
  PRESENT_SRC
};

// Layout of an image state. All uses in one image state map to one layout; USE_NONE is UNDEFINED.
Layout layout_of(uint32_t uses);

} // namespace vkc

// Per-device state of the active mode (e.g. vk11's render pass + framebuffer caches); each mode derives its own and
// downcasts. The mode destroys its Vulkan objects in its device_shutdown, before the device; the destructor frees
// memory.
struct GpuModeData {
  virtual ~GpuModeData() = default;
};

struct GpuDevice {
  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props = {};
  uint32_t api = 0; // instance apiVersion (the mode's version)
  // VkPhysicalDeviceDriverProperties of physical (pNext cleared), queried when the effective version is 1.2+ or the
  // device exposes VK_KHR_driver_properties (nothing is enabled for it); driver_props = false: not available, zeroed
  bool driver_props = false;
  VkPhysicalDeviceDriverProperties driver = {};
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VmaAllocator vma = nullptr;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  bool validation = false;
  bool display_timing = false;      // VK_GOOGLE_display_timing enabled (refresh period query)
  bool multi_draw_indirect = false; // multiDrawIndirect + drawIndirectFirstInstance enabled (indirect draw_count > 1)
  // Surface rotation the app applies itself (swapchain preTransform = currentTransform): fixed by the first swapchain
  // create, baked into every vertex shader's view_orientation()
  VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  bool transform_set = false;
  // Sampler of every `texture` binding: linear min/mag/mip, clamp to edge, all mips, no anisotropy
  VkSampler sampler = VK_NULL_HANDLE;
  // Tracy GPU context and the command pool/buffer it uses (alive for the context lifetime)
  VkCommandPool tracy_pool = VK_NULL_HANDLE;
  VkCommandBuffer tracy_cmd = VK_NULL_HANDLE;
  TracyVkCtx tracy = nullptr;
  std::unique_ptr<GpuModeData> mode; // null when the mode has no per-device state
};

constexpr uint32_t PXB_FRAMES_IN_FLIGHT = 2;
constexpr uint32_t PXB_MAX_MIPS = 16;

enum GpuBufferUsage : uint32_t {
  GPU_BUF_VERTEX = 1,
  GPU_BUF_INDEX = 2,
  GPU_BUF_UNIFORM = 4,
  GPU_BUF_STORAGE = 8,
  GPU_BUF_INDIRECT = 16,
};

enum class GpuBufferMode {
  staged, // DEVICE_LOCAL buffer + own persistent staging buffer per frame slot; close() records a copy
  mapped, // HOST_VISIBLE|HOST_COHERENT, persistently mapped; write() is a memcpy
};

struct GpuBufferDesc {
  VkDeviceSize size = 0;
  uint32_t usage = 0; // GpuBufferUsage flags
  GpuBufferMode mode = GpuBufferMode::staged;
};

struct GpuBuffer {
  VkBuffer h = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;
  VkDeviceSize size = 0;
  uint32_t usage = 0;
  GpuBufferMode mode = GpuBufferMode::staged;
  void* mapped = nullptr; // mapped mode: the buffer
  vkc::State state;
  // staged mode only: one staging buffer per frame slot, created on the first open in that slot, so a buffer can be
  // rewritten every frame while earlier frames' copies are still in flight
  VkBuffer staging[PXB_FRAMES_IN_FLIGHT] = {};
  VmaAllocation staging_alloc[PXB_FRAMES_IN_FLIGHT] = {};
  void* staging_map[PXB_FRAMES_IN_FLIGHT] = {};
  // write session
  bool is_open = false;
  uint32_t slot = 0;           // staging slot of the open session (staged)
  VkDeviceSize lo = 0, hi = 0; // written range [lo, hi) since open (staged)
};

enum class GpuTextureKind { color, depth };

struct GpuTextureDesc {
  uint32_t width = 0, height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  GpuTextureKind kind = GpuTextureKind::color;
  uint32_t mip_levels = 1; // 1..PXB_MAX_MIPS, at most the full chain
  bool storage = false;    // color only: adds STORAGE usage and one view per mip (format must support storage)
};

struct GpuTexture {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;        // all mips (sampling, attachments when 1 mip)
  VkImageView mip_views[PXB_MAX_MIPS] = {}; // storage only: one view per mip
  VmaAllocation alloc = nullptr;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkExtent2D extent = {};
  GpuTextureKind kind = GpuTextureKind::color;
  uint32_t mip_levels = 1;
  bool storage = false;
  vkc::State state[PXB_MAX_MIPS]; // per mip
};

enum class GpuShaderStage { vertex, fragment, compute };

// Shape-driven std430 block layout (push constants today; UBO/SSBO later). Built and packed by vkc/pipeline.{h,cpp}.
enum class GpuFieldType { float1, vec2, vec3, vec4, mat4, uint1, uvec4 };

struct GpuFieldDecl {
  std::string name;
  std::string type; // "float" | "vec2" | "vec3" | "vec4" | "mat4" | "uint" | "uvec4"
};

struct GpuLayoutField {
  std::string name;
  GpuFieldType type = GpuFieldType::float1;
  uint32_t offset = 0;
};

struct GpuLayout {
  std::string instance;               // GLSL instance name, e.g. "push"
  std::vector<GpuLayoutField> fields; // in layout order (size descending, then name ascending)
  uint32_t size = 0;                  // bytes, rounded up to the block's max alignment; 0 = empty
};

enum class GpuVertexFormat { float1, float2, float3, float4, unorm4 }; // unorm4: 4 x u8 read as vec4 in [0, 1]
enum class GpuTopology { triangles, lines, points };
enum class GpuCull { none, back, front };
// alpha: color = src * SRC_ALPHA + dst * (1 - SRC_ALPHA), alpha = src + dst * (1 - SRC_ALPHA)
enum class GpuBlend { none, alpha };

// texture: sampler2D (COMBINED_IMAGE_SAMPLER, SHADER_READ_ONLY_OPTIMAL, device sampler)
// image: rgba8 image2D (STORAGE_IMAGE, GENERAL)
enum class GpuBindingKind { texture, image };

struct GpuBindingDecl {
  std::string name; // GLSL identifier
  GpuBindingKind kind;
};

struct GpuPipelineDesc {
  const char* vs = nullptr; // GLSL sources (may carry their own #version, default 450)
  const char* fs = nullptr;
  const char* cs = nullptr;   // compute source: exclusive with vs/fs; declares its own local_size; push range COMPUTE
  const char* vs_name = "vs"; // names used in compile errors
  const char* fs_name = "fs";
  const char* cs_name = "cs";
  GpuLayout push;                               // prelude is injected into both shaders; range stages VERTEX|FRAGMENT
  std::vector<GpuVertexFormat> vertex_layout;   // binding 0, per vertex, locations 0..n-1
  std::vector<GpuVertexFormat> instance_layout; // binding 1, per instance, locations n.. (empty = no binding)
  std::vector<GpuBindingDecl> bindings;         // set 0, binding = index, stages ALL; prelude in both shaders
  GpuTopology topology = GpuTopology::triangles;
  GpuCull cull = GpuCull::none; // front face is counter-clockwise
  GpuBlend blend = GpuBlend::none;
  bool depth_test = false; // compare LESS_OR_EQUAL
  bool depth_write = false;
  VkFormat color_format = VK_FORMAT_UNDEFINED;
  VkFormat depth_format = VK_FORMAT_UNDEFINED; // UNDEFINED = no depth attachment (depth test/write must be off)
};

struct GpuPipeline {
  VkPipeline h = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  GpuLayout push;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE; // set 0; null when bindings is empty
  std::vector<GpuBindingDecl> bindings;
  bool compute = false; // created from desc.cs (bind point COMPUTE); graphics otherwise
};

// ImGui renderer (vkc/imgui.h). Buffers are kept per slot: the PXB_FRAMES_IN_FLIGHT frame slots (frame command
// buffer, reused after frame_begin waits its fence) plus the offscreen blit (blit command buffer, reused after
// blit_begin waits its fence).
constexpr uint32_t GPU_IMGUI_SLOTS = PXB_FRAMES_IN_FLIGHT + 1;
constexpr uint32_t GPU_IMGUI_BLIT_SLOT = PXB_FRAMES_IN_FLIGHT;
constexpr uint32_t GPU_IMGUI_MAX_TEXTURES = 8; // live + not yet freed ImGui textures (descriptor pool size)

// One ImTextureData on the GPU (RGBA8 UNORM, 1 mip). ImTextureData::TexID points to it.
struct GpuImGuiTexture {
  GpuTexture tex;
  VkDescriptorSet set = VK_NULL_HANDLE; // texture binding of GpuImGui::pipe, from GpuImGui::pool
  uint32_t used = 0;                    // bit per slot: drawn in that slot since the slot's fence was last waited
  bool dead = false;                    // ImGui destroyed it; freed once used is 0
};

// Zero-initialized value is valid (not initialized).
struct GpuImGui {
  GpuPipeline pipe;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; // baked into pipe (scissor rotation)
  VkDescriptorPool pool = VK_NULL_HANDLE;
  std::vector<std::unique_ptr<GpuImGuiTexture>> textures;
  GpuBuffer vertex[GPU_IMGUI_SLOTS];               // mapped, grown as needed
  GpuBuffer index[GPU_IMGUI_SLOTS];                // mapped, grown as needed
  std::vector<GpuBuffer> staging[GPU_IMGUI_SLOTS]; // texture uploads recorded in the slot, freed when it is reused
  VkCommandBuffer prepared = VK_NULL_HANDLE;       // command buffer of the last imgui_prepare (imgui_draw must match)
  uint32_t slot = 0;                               // its slot
};

constexpr VkFormat GPU_PASS_DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

// One shared depth texture (swapchain-sized) plus the open pass. Zero-initialized value is valid.
struct GpuPassCtx {
  GpuTexture depth;
  VkCommandBuffer cmd = VK_NULL_HANDLE; // non-null while a pass is open
  VkExtent2D extent = {};               // render area of the open pass, as the app sees it (view_extent)
};

struct GpuPassDesc {
  bool clear = false;        // false = LOAD the swapchain image
  float clear_color[4] = {}; // r, g, b, a
  bool depth = false;        // attach the shared depth texture, cleared to 1.0, contents not stored
  bool swapchain = false;    // offscreen: target the acquired swapchain image instead of offscreen_tex (HUD)
};

enum class GpuIndexType { u16, u32 };

struct GpuDrawDesc {
  const GpuPipeline* pipeline = nullptr;
  GpuBuffer* vertex_buffers[2] = {}; // [0] per-vertex (required), [1] per-instance (optional), offset 0
  GpuBuffer* index_buffer = nullptr; // offset 0
  GpuIndexType index_type = GpuIndexType::u16;
  uint32_t index_count = 0;
  uint32_t instance_count = 1;
  // indirect: vkCmdDrawIndexedIndirect over draw_count tightly packed VkDrawIndexedIndirectCommand at offset 0
  // (index_count / instance_count ignored)
  GpuBuffer* indirect = nullptr;
  uint32_t draw_count = 0;
  const void* push = nullptr; // pipeline->push.size bytes (vk13::layout_pack); required when that size > 0
};

// Transient descriptor sets: per frame slot, a chain of pools (created lazily) reset when the slot is reused.
struct GpuDescriptorPools {
  std::vector<VkDescriptorPool> pools[PXB_FRAMES_IN_FLIGHT];
  uint32_t current[PXB_FRAMES_IN_FLIGHT] = {}; // pool allocated from in each slot
};

struct GpuFrame {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;              // signaled when this slot's submit completes
  VkSemaphore image_acquired = VK_NULL_HANDLE; // acquire -> submit
};

struct GpuSwapchain {
  VkSwapchainKHR swapchain = VK_NULL_HANDLE; // VK_NULL_HANDLE while extent is zero (minimized)
  VkSurfaceFormatKHR format = {};
  VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
  VkExtent2D extent = {};      // image size, native orientation (rotated by 90/270 vs view_extent)
  VkExtent2D view_extent = {}; // size as the app sees it (scripts, HUD)
  uint64_t refresh_ns = 0;     // display refresh period (VK_GOOGLE_display_timing), 0 = unknown
  std::vector<VkImage> images;
  std::vector<VkImageView> views;
  std::vector<vkc::State> image_states;     // per swapchain image, tracked by ensure_state
  std::vector<VkSemaphore> render_finished; // per swapchain image: submit -> present
  GpuFrame frames[PXB_FRAMES_IN_FLIGHT];
  uint32_t frame = 0;    // current frame slot
  uint32_t image = 0;    // image acquired by frame_begin / blit_begin
  bool acquired = false; // a swapchain image is held (presented by frame end / blit end)
  // --offscreen: passes render into offscreen_tex (swapchain extent + format, recreated with the swapchain); frames
  // never acquire or present. capture copies the last submitted frame into capture, blit_* later copies capture
  // to the screen, both in their own submits. Set before the first swapchain_create.
  bool offscreen = false;
  bool readable = false; // --screenshot: images get TRANSFER_SRC. Set before the first create
  GpuTexture offscreen_tex;
  GpuTexture capture;          // offscreen: frame waiting to be blitted (same extent + format)
  GpuFrame blit;               // offscreen capture + blit: own pool/cmd/fence/acquire semaphore
  bool needs_recreate = false; // set on zero extent, OUT_OF_DATE or SUBOPTIMAL
  bool submit_failed = false;  // queue submit failed (OOM / device lost): fatal, no more frames
  bool printed = false;
};

// --screenshot: one frame's render target read back to the host.
struct GpuScreenshot {
  VkBuffer buf = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;
  void* mapped = nullptr;
};

constexpr uint32_t PXB_TIMER_QUERIES = 256; // per frame slot; 2 used by the automatic frame timer

struct GpuTimerOpen {
  uint32_t id = 0; // user timer index
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  uint32_t query = UINT32_MAX;             // begin query, end = query + 1; UINT32_MAX when dropped (pool full)
  std::unique_ptr<tracy::VkCtxScope> zone; // set with TRACY_ENABLE only
};

struct GpuTimerRecord {
  uint32_t id = 0;    // user timer index
  uint32_t query = 0; // begin query, end = query + 1
};

// User timer, created once by name; results accumulate by index.
struct GpuUserTimer {
  std::string name;
  std::vector<double> ns;    // per occurrence
  const void* loc = nullptr; // tracy::SourceLocationData* (TRACY_ENABLE only), never freed
};

// Per frame slot. Query 0 = frame start, query 1 = frame end.
struct GpuTimerSlot {
  VkQueryPool pool = VK_NULL_HANDLE;
  uint32_t used = 0;
  uint64_t seq = 0;            // frame order
  bool pending = false;        // recorded, not resolved yet
  bool ended = false;          // frame_end hook ran
  bool break_interval = false; // offscreen blit after this frame: the interval to the next frame is dropped
  bool drop = false;           // timers_drop_frame: busy and user timer results are dropped (reset when resolved)
  bool overflow_printed = false;
  std::vector<GpuTimerRecord> records; // closed user timers
};

// Zero-initialized value is valid. Timers are no-ops (results empty) when timestamps are unsupported.
struct GpuTimers {
  bool enabled = false;
  TracyVkCtx tracy = nullptr; // from the device, for the user timer zones
  double period_ns = 0.0;
  uint64_t mask = 0;
  GpuTimerSlot slots[PXB_FRAMES_IN_FLIGHT];
  uint32_t cur = 0;                     // slot of the open frame
  VkCommandBuffer cmd = VK_NULL_HANDLE; // non-null between frame_begin and frame_end
  uint64_t next_seq = 1;
  std::vector<GpuTimerOpen> open;                // LIFO
  std::unique_ptr<tracy::VkCtxScope> frame_zone; // `gpu_frame` over the frame cmd (set with TRACY_ENABLE only)
  bool has_prev = false;
  uint64_t prev_start = 0;
  std::vector<double> busy_ns;     // per frame, end - start
  std::vector<double> interval_ns; // per frame, start(n) - start(n-1)
  std::vector<GpuUserTimer> user;  // creation order
};
