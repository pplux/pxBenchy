#include "gpu/vk13.h"
#include "gpu/vkc/info.h"

// Vulkan 1.3: every 1.1 / 1.2 / 1.3 feature and property in VkPhysicalDeviceVulkan11/12/13Features / Properties.
#define F(f) j->put(#f, s.f)
#define B(f) j->boolean(#f, s.f)

static void put(vkc::Json* j, const VkPhysicalDeviceVulkan11Features& s) {
  j->open("VkPhysicalDeviceVulkan11Features");
  B(storageBuffer16BitAccess);
  B(uniformAndStorageBuffer16BitAccess);
  B(storagePushConstant16);
  B(storageInputOutput16);
  B(multiview);
  B(multiviewGeometryShader);
  B(multiviewTessellationShader);
  B(variablePointersStorageBuffer);
  B(variablePointers);
  B(protectedMemory);
  B(samplerYcbcrConversion);
  B(shaderDrawParameters);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceVulkan12Features& s) {
  j->open("VkPhysicalDeviceVulkan12Features");
  B(samplerMirrorClampToEdge);
  B(drawIndirectCount);
  B(storageBuffer8BitAccess);
  B(uniformAndStorageBuffer8BitAccess);
  B(storagePushConstant8);
  B(shaderBufferInt64Atomics);
  B(shaderSharedInt64Atomics);
  B(shaderFloat16);
  B(shaderInt8);
  B(descriptorIndexing);
  B(shaderInputAttachmentArrayDynamicIndexing);
  B(shaderUniformTexelBufferArrayDynamicIndexing);
  B(shaderStorageTexelBufferArrayDynamicIndexing);
  B(shaderUniformBufferArrayNonUniformIndexing);
  B(shaderSampledImageArrayNonUniformIndexing);
  B(shaderStorageBufferArrayNonUniformIndexing);
  B(shaderStorageImageArrayNonUniformIndexing);
  B(shaderInputAttachmentArrayNonUniformIndexing);
  B(shaderUniformTexelBufferArrayNonUniformIndexing);
  B(shaderStorageTexelBufferArrayNonUniformIndexing);
  B(descriptorBindingUniformBufferUpdateAfterBind);
  B(descriptorBindingSampledImageUpdateAfterBind);
  B(descriptorBindingStorageImageUpdateAfterBind);
  B(descriptorBindingStorageBufferUpdateAfterBind);
  B(descriptorBindingUniformTexelBufferUpdateAfterBind);
  B(descriptorBindingStorageTexelBufferUpdateAfterBind);
  B(descriptorBindingUpdateUnusedWhilePending);
  B(descriptorBindingPartiallyBound);
  B(descriptorBindingVariableDescriptorCount);
  B(runtimeDescriptorArray);
  B(samplerFilterMinmax);
  B(scalarBlockLayout);
  B(imagelessFramebuffer);
  B(uniformBufferStandardLayout);
  B(shaderSubgroupExtendedTypes);
  B(separateDepthStencilLayouts);
  B(hostQueryReset);
  B(timelineSemaphore);
  B(bufferDeviceAddress);
  B(bufferDeviceAddressCaptureReplay);
  B(bufferDeviceAddressMultiDevice);
  B(vulkanMemoryModel);
  B(vulkanMemoryModelDeviceScope);
  B(vulkanMemoryModelAvailabilityVisibilityChains);
  B(shaderOutputViewportIndex);
  B(shaderOutputLayer);
  B(subgroupBroadcastDynamicId);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceVulkan13Features& s) {
  j->open("VkPhysicalDeviceVulkan13Features");
  B(robustImageAccess);
  B(inlineUniformBlock);
  B(descriptorBindingInlineUniformBlockUpdateAfterBind);
  B(pipelineCreationCacheControl);
  B(privateData);
  B(shaderDemoteToHelperInvocation);
  B(shaderTerminateInvocation);
  B(subgroupSizeControl);
  B(computeFullSubgroups);
  B(synchronization2);
  B(textureCompressionASTC_HDR);
  B(shaderZeroInitializeWorkgroupMemory);
  B(dynamicRendering);
  B(shaderIntegerDotProduct);
  B(maintenance4);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceVulkan11Properties& s) {
  j->open("VkPhysicalDeviceVulkan11Properties");
  F(deviceUUID);
  F(driverUUID);
  F(deviceLUID);
  F(deviceNodeMask);
  B(deviceLUIDValid);
  F(subgroupSize);
  F(subgroupSupportedStages);
  F(subgroupSupportedOperations);
  B(subgroupQuadOperationsInAllStages);
  F(pointClippingBehavior);
  F(maxMultiviewViewCount);
  F(maxMultiviewInstanceIndex);
  B(protectedNoFault);
  F(maxPerSetDescriptors);
  F(maxMemoryAllocationSize);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceVulkan12Properties& s) {
  j->open("VkPhysicalDeviceVulkan12Properties");
  F(driverID);
  F(driverName);
  F(driverInfo);
  F(conformanceVersion);
  F(denormBehaviorIndependence);
  F(roundingModeIndependence);
  B(shaderSignedZeroInfNanPreserveFloat16);
  B(shaderSignedZeroInfNanPreserveFloat32);
  B(shaderSignedZeroInfNanPreserveFloat64);
  B(shaderDenormPreserveFloat16);
  B(shaderDenormPreserveFloat32);
  B(shaderDenormPreserveFloat64);
  B(shaderDenormFlushToZeroFloat16);
  B(shaderDenormFlushToZeroFloat32);
  B(shaderDenormFlushToZeroFloat64);
  B(shaderRoundingModeRTEFloat16);
  B(shaderRoundingModeRTEFloat32);
  B(shaderRoundingModeRTEFloat64);
  B(shaderRoundingModeRTZFloat16);
  B(shaderRoundingModeRTZFloat32);
  B(shaderRoundingModeRTZFloat64);
  F(maxUpdateAfterBindDescriptorsInAllPools);
  B(shaderUniformBufferArrayNonUniformIndexingNative);
  B(shaderSampledImageArrayNonUniformIndexingNative);
  B(shaderStorageBufferArrayNonUniformIndexingNative);
  B(shaderStorageImageArrayNonUniformIndexingNative);
  B(shaderInputAttachmentArrayNonUniformIndexingNative);
  B(robustBufferAccessUpdateAfterBind);
  B(quadDivergentImplicitLod);
  F(maxPerStageDescriptorUpdateAfterBindSamplers);
  F(maxPerStageDescriptorUpdateAfterBindUniformBuffers);
  F(maxPerStageDescriptorUpdateAfterBindStorageBuffers);
  F(maxPerStageDescriptorUpdateAfterBindSampledImages);
  F(maxPerStageDescriptorUpdateAfterBindStorageImages);
  F(maxPerStageDescriptorUpdateAfterBindInputAttachments);
  F(maxPerStageUpdateAfterBindResources);
  F(maxDescriptorSetUpdateAfterBindSamplers);
  F(maxDescriptorSetUpdateAfterBindUniformBuffers);
  F(maxDescriptorSetUpdateAfterBindUniformBuffersDynamic);
  F(maxDescriptorSetUpdateAfterBindStorageBuffers);
  F(maxDescriptorSetUpdateAfterBindStorageBuffersDynamic);
  F(maxDescriptorSetUpdateAfterBindSampledImages);
  F(maxDescriptorSetUpdateAfterBindStorageImages);
  F(maxDescriptorSetUpdateAfterBindInputAttachments);
  F(supportedDepthResolveModes);
  F(supportedStencilResolveModes);
  B(independentResolveNone);
  B(independentResolve);
  B(filterMinmaxSingleComponentFormats);
  B(filterMinmaxImageComponentMapping);
  F(maxTimelineSemaphoreValueDifference);
  F(framebufferIntegerColorSampleCounts);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceVulkan13Properties& s) {
  j->open("VkPhysicalDeviceVulkan13Properties");
  F(minSubgroupSize);
  F(maxSubgroupSize);
  F(maxComputeWorkgroupSubgroups);
  F(requiredSubgroupSizeStages);
  F(maxInlineUniformBlockSize);
  F(maxPerStageDescriptorInlineUniformBlocks);
  F(maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks);
  F(maxDescriptorSetInlineUniformBlocks);
  F(maxDescriptorSetUpdateAfterBindInlineUniformBlocks);
  F(maxInlineUniformTotalSize);
  B(integerDotProduct8BitUnsignedAccelerated);
  B(integerDotProduct8BitSignedAccelerated);
  B(integerDotProduct8BitMixedSignednessAccelerated);
  B(integerDotProduct4x8BitPackedUnsignedAccelerated);
  B(integerDotProduct4x8BitPackedSignedAccelerated);
  B(integerDotProduct4x8BitPackedMixedSignednessAccelerated);
  B(integerDotProduct16BitUnsignedAccelerated);
  B(integerDotProduct16BitSignedAccelerated);
  B(integerDotProduct16BitMixedSignednessAccelerated);
  B(integerDotProduct32BitUnsignedAccelerated);
  B(integerDotProduct32BitSignedAccelerated);
  B(integerDotProduct32BitMixedSignednessAccelerated);
  B(integerDotProduct64BitUnsignedAccelerated);
  B(integerDotProduct64BitSignedAccelerated);
  B(integerDotProduct64BitMixedSignednessAccelerated);
  B(integerDotProductAccumulatingSaturating8BitUnsignedAccelerated);
  B(integerDotProductAccumulatingSaturating8BitSignedAccelerated);
  B(integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated);
  B(integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated);
  B(integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated);
  B(integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated);
  B(integerDotProductAccumulatingSaturating16BitUnsignedAccelerated);
  B(integerDotProductAccumulatingSaturating16BitSignedAccelerated);
  B(integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated);
  B(integerDotProductAccumulatingSaturating32BitUnsignedAccelerated);
  B(integerDotProductAccumulatingSaturating32BitSignedAccelerated);
  B(integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated);
  B(integerDotProductAccumulatingSaturating64BitUnsignedAccelerated);
  B(integerDotProductAccumulatingSaturating64BitSignedAccelerated);
  B(integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated);
  F(storageTexelBufferOffsetAlignmentBytes);
  B(storageTexelBufferOffsetSingleTexelAlignment);
  F(uniformTexelBufferOffsetAlignmentBytes);
  B(uniformTexelBufferOffsetSingleTexelAlignment);
  F(maxBufferSize);
  j->close();
}

#undef F
#undef B

namespace vk13 {

std::string info_json(int depth) {
  VolkInstanceTable t;
  VkInstance instance = vkc::part::probe_instance_create(VK_API_VERSION_1_3, &t);
  if (!instance) { return "null"; }
  vkc::Json j;
  j.depth = depth;
  j.open(nullptr);
  vkc::part::info_instance(&j);
  j.open("devices", '[');
  for (VkPhysicalDevice pd : vkc::part::physical_devices(instance, &t, VK_API_VERSION_1_3)) {
    vkc::part::info_device_begin(&j, pd, &t);

    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f11};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
    t.vkGetPhysicalDeviceFeatures2(pd, &f2);
    put(&j, f11);
    put(&j, f12);
    put(&j, f13);

    VkPhysicalDeviceVulkan11Properties p11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &p11};
    VkPhysicalDeviceVulkan13Properties p13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, &p12};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p13};
    t.vkGetPhysicalDeviceProperties2(pd, &p2);
    put(&j, p11);
    put(&j, p12);
    put(&j, p13);

    vkc::part::info_device_end(&j);
  }
  j.close(']');
  j.close();
  t.vkDestroyInstance(instance, nullptr);
  return j.out;
}

} // namespace vk13
