#include "gpu/vkc/info.h"
#include <vector>
#include "json.h"

namespace vkc {

void Json::key(const char* k) {
  if (!out.empty()) {
    out += first ? "\n" : ",\n";
    out.append(2 * depth, ' ');
  }
  first = false;
  if (k) {
    out += '"';
    out += k;
    out += "\": ";
  }
}

void Json::open(const char* k, char c) {
  key(k);
  out += c;
  ++depth;
  first = true;
}

void Json::close(char c) {
  --depth;
  if (!first) {
    out += '\n';
    out.append(2 * depth, ' ');
  }
  out += c;
  first = false;
}

void Json::boolean(const char* k, VkBool32 v) {
  key(k);
  out += v ? "true" : "false";
}

void Json::str(const char* k, const char* v) {
  key(k);
  out += json_string(v);
}

void Json::version(const char* k, uint32_t v) {
  key(k);
  str_appendf(&out, "\"%u.%u.%u\"", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

void Json::put(const char* k, const VkConformanceVersion& v) {
  key(k);
  str_appendf(&out, "\"%u.%u.%u.%u\"", v.major, v.minor, v.subminor, v.patch);
}

void Json::number_text(long long v) { str_appendf(&out, "%lld", v); }
void Json::number_text(unsigned long long v) { str_appendf(&out, "%llu", v); }
void Json::number_text(double v) { str_appendf(&out, "%.9g", v); }
void Json::number_text(uint8_t v, int) { str_appendf(&out, "%02x", v); }

} // namespace vkc

#define F(f) j->put(#f, s.f)
#define B(f) j->boolean(#f, s.f)

static void put_limits(vkc::Json* j, const VkPhysicalDeviceLimits& s) {
  j->open("limits");
  F(maxImageDimension1D);
  F(maxImageDimension2D);
  F(maxImageDimension3D);
  F(maxImageDimensionCube);
  F(maxImageArrayLayers);
  F(maxTexelBufferElements);
  F(maxUniformBufferRange);
  F(maxStorageBufferRange);
  F(maxPushConstantsSize);
  F(maxMemoryAllocationCount);
  F(maxSamplerAllocationCount);
  F(bufferImageGranularity);
  F(sparseAddressSpaceSize);
  F(maxBoundDescriptorSets);
  F(maxPerStageDescriptorSamplers);
  F(maxPerStageDescriptorUniformBuffers);
  F(maxPerStageDescriptorStorageBuffers);
  F(maxPerStageDescriptorSampledImages);
  F(maxPerStageDescriptorStorageImages);
  F(maxPerStageDescriptorInputAttachments);
  F(maxPerStageResources);
  F(maxDescriptorSetSamplers);
  F(maxDescriptorSetUniformBuffers);
  F(maxDescriptorSetUniformBuffersDynamic);
  F(maxDescriptorSetStorageBuffers);
  F(maxDescriptorSetStorageBuffersDynamic);
  F(maxDescriptorSetSampledImages);
  F(maxDescriptorSetStorageImages);
  F(maxDescriptorSetInputAttachments);
  F(maxVertexInputAttributes);
  F(maxVertexInputBindings);
  F(maxVertexInputAttributeOffset);
  F(maxVertexInputBindingStride);
  F(maxVertexOutputComponents);
  F(maxTessellationGenerationLevel);
  F(maxTessellationPatchSize);
  F(maxTessellationControlPerVertexInputComponents);
  F(maxTessellationControlPerVertexOutputComponents);
  F(maxTessellationControlPerPatchOutputComponents);
  F(maxTessellationControlTotalOutputComponents);
  F(maxTessellationEvaluationInputComponents);
  F(maxTessellationEvaluationOutputComponents);
  F(maxGeometryShaderInvocations);
  F(maxGeometryInputComponents);
  F(maxGeometryOutputComponents);
  F(maxGeometryOutputVertices);
  F(maxGeometryTotalOutputComponents);
  F(maxFragmentInputComponents);
  F(maxFragmentOutputAttachments);
  F(maxFragmentDualSrcAttachments);
  F(maxFragmentCombinedOutputResources);
  F(maxComputeSharedMemorySize);
  F(maxComputeWorkGroupCount);
  F(maxComputeWorkGroupInvocations);
  F(maxComputeWorkGroupSize);
  F(subPixelPrecisionBits);
  F(subTexelPrecisionBits);
  F(mipmapPrecisionBits);
  F(maxDrawIndexedIndexValue);
  F(maxDrawIndirectCount);
  F(maxSamplerLodBias);
  F(maxSamplerAnisotropy);
  F(maxViewports);
  F(maxViewportDimensions);
  F(viewportBoundsRange);
  F(viewportSubPixelBits);
  F(minMemoryMapAlignment);
  F(minTexelBufferOffsetAlignment);
  F(minUniformBufferOffsetAlignment);
  F(minStorageBufferOffsetAlignment);
  F(minTexelOffset);
  F(maxTexelOffset);
  F(minTexelGatherOffset);
  F(maxTexelGatherOffset);
  F(minInterpolationOffset);
  F(maxInterpolationOffset);
  F(subPixelInterpolationOffsetBits);
  F(maxFramebufferWidth);
  F(maxFramebufferHeight);
  F(maxFramebufferLayers);
  F(framebufferColorSampleCounts);
  F(framebufferDepthSampleCounts);
  F(framebufferStencilSampleCounts);
  F(framebufferNoAttachmentsSampleCounts);
  F(maxColorAttachments);
  F(sampledImageColorSampleCounts);
  F(sampledImageIntegerSampleCounts);
  F(sampledImageDepthSampleCounts);
  F(sampledImageStencilSampleCounts);
  F(storageImageSampleCounts);
  F(maxSampleMaskWords);
  B(timestampComputeAndGraphics);
  F(timestampPeriod);
  F(maxClipDistances);
  F(maxCullDistances);
  F(maxCombinedClipAndCullDistances);
  F(discreteQueuePriorities);
  F(pointSizeRange);
  F(lineWidthRange);
  F(pointSizeGranularity);
  F(lineWidthGranularity);
  B(strictLines);
  B(standardSampleLocations);
  F(optimalBufferCopyOffsetAlignment);
  F(optimalBufferCopyRowPitchAlignment);
  F(nonCoherentAtomSize);
  j->close();
}

static void put_sparse(vkc::Json* j, const VkPhysicalDeviceSparseProperties& s) {
  j->open("sparseProperties");
  B(residencyStandard2DBlockShape);
  B(residencyStandard2DMultisampleBlockShape);
  B(residencyStandard3DBlockShape);
  B(residencyAlignedMipSize);
  B(residencyNonResidentStrict);
  j->close();
}

static void put_features(vkc::Json* j, const VkPhysicalDeviceFeatures& s) {
  j->open("features");
  B(robustBufferAccess);
  B(fullDrawIndexUint32);
  B(imageCubeArray);
  B(independentBlend);
  B(geometryShader);
  B(tessellationShader);
  B(sampleRateShading);
  B(dualSrcBlend);
  B(logicOp);
  B(multiDrawIndirect);
  B(drawIndirectFirstInstance);
  B(depthClamp);
  B(depthBiasClamp);
  B(fillModeNonSolid);
  B(depthBounds);
  B(wideLines);
  B(largePoints);
  B(alphaToOne);
  B(multiViewport);
  B(samplerAnisotropy);
  B(textureCompressionETC2);
  B(textureCompressionASTC_LDR);
  B(textureCompressionBC);
  B(occlusionQueryPrecise);
  B(pipelineStatisticsQuery);
  B(vertexPipelineStoresAndAtomics);
  B(fragmentStoresAndAtomics);
  B(shaderTessellationAndGeometryPointSize);
  B(shaderImageGatherExtended);
  B(shaderStorageImageExtendedFormats);
  B(shaderStorageImageMultisample);
  B(shaderStorageImageReadWithoutFormat);
  B(shaderStorageImageWriteWithoutFormat);
  B(shaderUniformBufferArrayDynamicIndexing);
  B(shaderSampledImageArrayDynamicIndexing);
  B(shaderStorageBufferArrayDynamicIndexing);
  B(shaderStorageImageArrayDynamicIndexing);
  B(shaderClipDistance);
  B(shaderCullDistance);
  B(shaderFloat64);
  B(shaderInt64);
  B(shaderInt16);
  B(shaderResourceResidency);
  B(shaderResourceMinLod);
  B(sparseBinding);
  B(sparseResidencyBuffer);
  B(sparseResidencyImage2D);
  B(sparseResidencyImage3D);
  B(sparseResidency2Samples);
  B(sparseResidency4Samples);
  B(sparseResidency8Samples);
  B(sparseResidency16Samples);
  B(sparseResidencyAliased);
  B(variableMultisampleRate);
  B(inheritedQueries);
  j->close();
}

#undef F
#undef B

namespace vkc::part {

void info_instance(Json* j) {
  uint32_t version = 0;
  vkEnumerateInstanceVersion(&version);
  j->version("instance_version", version);
  uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> exts(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
  j->open("instance_extensions");
  for (const VkExtensionProperties& e : exts) { j->put(e.extensionName, e.specVersion); }
  j->close();
  count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  j->open("layers", '[');
  for (const VkLayerProperties& l : layers) {
    j->open(nullptr);
    j->str("layerName", l.layerName);
    j->version("specVersion", l.specVersion);
    j->put("implementationVersion", l.implementationVersion);
    j->str("description", l.description);
    j->close();
  }
  j->close(']');
}

void info_device_begin(Json* j, VkPhysicalDevice pd, const VolkInstanceTable* t) {
  VkPhysicalDeviceProperties p;
  t->vkGetPhysicalDeviceProperties(pd, &p);
  j->open(nullptr);
  j->version("apiVersion", p.apiVersion);
  j->put("driverVersion", p.driverVersion);
  j->put("vendorID", p.vendorID);
  j->put("deviceID", p.deviceID);
  j->put("deviceType", p.deviceType);
  j->put("deviceName", p.deviceName);
  j->put("pipelineCacheUUID", p.pipelineCacheUUID);
  put_limits(j, p.limits);
  put_sparse(j, p.sparseProperties);

  VkPhysicalDeviceFeatures f;
  t->vkGetPhysicalDeviceFeatures(pd, &f);
  put_features(j, f);

  VkPhysicalDeviceMemoryProperties m;
  t->vkGetPhysicalDeviceMemoryProperties(pd, &m);
  j->open("memoryHeaps", '[');
  for (uint32_t i = 0; i < m.memoryHeapCount; ++i) {
    j->open(nullptr);
    j->put("size", m.memoryHeaps[i].size);
    j->put("flags", m.memoryHeaps[i].flags);
    j->close();
  }
  j->close(']');
  j->open("memoryTypes", '[');
  for (uint32_t i = 0; i < m.memoryTypeCount; ++i) {
    j->open(nullptr);
    j->put("heapIndex", m.memoryTypes[i].heapIndex);
    j->put("propertyFlags", m.memoryTypes[i].propertyFlags);
    j->close();
  }
  j->close(']');

  uint32_t count = 0;
  t->vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
  std::vector<VkQueueFamilyProperties> queues(count);
  t->vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, queues.data());
  j->open("queueFamilies", '[');
  for (const VkQueueFamilyProperties& q : queues) {
    const VkExtent3D& g = q.minImageTransferGranularity;
    uint32_t granularity[3] = {g.width, g.height, g.depth};
    j->open(nullptr);
    j->put("queueFlags", q.queueFlags);
    j->put("queueCount", q.queueCount);
    j->put("timestampValidBits", q.timestampValidBits);
    j->put("minImageTransferGranularity", granularity);
    j->close();
  }
  j->close(']');

  count = 0;
  t->vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> exts(count);
  t->vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, exts.data());
  j->open("extensions");
  for (const VkExtensionProperties& e : exts) { j->put(e.extensionName, e.specVersion); }
  j->close();

  // keyed by the VkFormat value; [linearTilingFeatures, optimalTilingFeatures, bufferFeatures], unsupported left out
  j->open("formats");
  for (int fmt = VK_FORMAT_UNDEFINED + 1; fmt <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; ++fmt) {
    VkFormatProperties fp;
    t->vkGetPhysicalDeviceFormatProperties(pd, (VkFormat)fmt, &fp);
    if (!fp.linearTilingFeatures && !fp.optimalTilingFeatures && !fp.bufferFeatures) { continue; }
    uint32_t flags[3] = {fp.linearTilingFeatures, fp.optimalTilingFeatures, fp.bufferFeatures};
    j->put(std::to_string(fmt).c_str(), flags);
  }
  j->close();
}

void info_device_end(Json* j) { j->close(); }

} // namespace vkc::part
