#include "gpu/vk11.h"
#include <cstring>
#include <vector>
#include "gpu/vkc/info.h"

// Vulkan 1.1 has no VkPhysicalDeviceVulkan11Features / Properties: each 1.1 feature and property struct is queried
// apart. Driver properties come from VK_KHR_driver_properties (core only in 1.2).
#define F(f) j->put(#f, s.f)
#define B(f) j->boolean(#f, s.f)

static void put(vkc::Json* j, const VkPhysicalDevice16BitStorageFeatures& s) {
  j->open("VkPhysicalDevice16BitStorageFeatures");
  B(storageBuffer16BitAccess);
  B(uniformAndStorageBuffer16BitAccess);
  B(storagePushConstant16);
  B(storageInputOutput16);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceMultiviewFeatures& s) {
  j->open("VkPhysicalDeviceMultiviewFeatures");
  B(multiview);
  B(multiviewGeometryShader);
  B(multiviewTessellationShader);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceVariablePointersFeatures& s) {
  j->open("VkPhysicalDeviceVariablePointersFeatures");
  B(variablePointersStorageBuffer);
  B(variablePointers);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceProtectedMemoryFeatures& s) {
  j->open("VkPhysicalDeviceProtectedMemoryFeatures");
  B(protectedMemory);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceSamplerYcbcrConversionFeatures& s) {
  j->open("VkPhysicalDeviceSamplerYcbcrConversionFeatures");
  B(samplerYcbcrConversion);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceShaderDrawParametersFeatures& s) {
  j->open("VkPhysicalDeviceShaderDrawParametersFeatures");
  B(shaderDrawParameters);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceIDProperties& s) {
  j->open("VkPhysicalDeviceIDProperties");
  F(deviceUUID);
  F(driverUUID);
  F(deviceLUID);
  F(deviceNodeMask);
  B(deviceLUIDValid);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceMaintenance3Properties& s) {
  j->open("VkPhysicalDeviceMaintenance3Properties");
  F(maxPerSetDescriptors);
  F(maxMemoryAllocationSize);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceMultiviewProperties& s) {
  j->open("VkPhysicalDeviceMultiviewProperties");
  F(maxMultiviewViewCount);
  F(maxMultiviewInstanceIndex);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDevicePointClippingProperties& s) {
  j->open("VkPhysicalDevicePointClippingProperties");
  F(pointClippingBehavior);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceProtectedMemoryProperties& s) {
  j->open("VkPhysicalDeviceProtectedMemoryProperties");
  B(protectedNoFault);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceSubgroupProperties& s) {
  j->open("VkPhysicalDeviceSubgroupProperties");
  F(subgroupSize);
  F(supportedStages);
  F(supportedOperations);
  B(quadOperationsInAllStages);
  j->close();
}

static void put(vkc::Json* j, const VkPhysicalDeviceDriverProperties& s) {
  j->open("VkPhysicalDeviceDriverProperties");
  F(driverID);
  F(driverName);
  F(driverInfo);
  F(conformanceVersion);
  j->close();
}

#undef F
#undef B

static bool has_driver_properties(VkPhysicalDevice pd, const VolkInstanceTable* t) {
  uint32_t count = 0;
  t->vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> exts(count);
  t->vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, exts.data());
  for (const VkExtensionProperties& e : exts) {
    if (strcmp(e.extensionName, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME) == 0) { return true; }
  }
  return false;
}

namespace vk11 {

std::string info_json(int depth) {
  VolkInstanceTable t;
  VkInstance instance = vkc::part::probe_instance_create(VK_API_VERSION_1_1, &t);
  if (!instance) { return "null"; }
  vkc::Json j;
  j.depth = depth;
  j.open(nullptr);
  vkc::part::info_instance(&j);
  j.open("devices", '[');
  for (VkPhysicalDevice pd : vkc::part::physical_devices(instance, &t, VK_API_VERSION_1_1)) {
    vkc::part::info_device_begin(&j, pd, &t);

    VkPhysicalDevice16BitStorageFeatures storage16 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceMultiviewFeatures multiview = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, &storage16};
    VkPhysicalDeviceVariablePointersFeatures pointers = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,
                                                         &multiview};
    VkPhysicalDeviceProtectedMemoryFeatures protect = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,
                                                       &pointers};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, &protect};
    VkPhysicalDeviceShaderDrawParametersFeatures draw_params = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, &ycbcr};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &draw_params};
    t.vkGetPhysicalDeviceFeatures2(pd, &f2);
    put(&j, storage16);
    put(&j, multiview);
    put(&j, pointers);
    put(&j, protect);
    put(&j, ycbcr);
    put(&j, draw_params);

    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceMaintenance3Properties maint3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, &id};
    VkPhysicalDeviceMultiviewProperties multiview_p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES, &maint3};
    VkPhysicalDevicePointClippingProperties clipping = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES,
                                                        &multiview_p};
    VkPhysicalDeviceProtectedMemoryProperties protect_p = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES, &clipping};
    VkPhysicalDeviceSubgroupProperties subgroup = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, &protect_p};
    VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &subgroup};
    bool has_driver = has_driver_properties(pd, &t);
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = has_driver ? (void*)&driver : (void*)&subgroup;
    t.vkGetPhysicalDeviceProperties2(pd, &p2);
    put(&j, id);
    put(&j, maint3);
    put(&j, multiview_p);
    put(&j, clipping);
    put(&j, protect_p);
    put(&j, subgroup);
    if (has_driver) { put(&j, driver); }

    vkc::part::info_device_end(&j);
  }
  j.close(']');
  j.close();
  t.vkDestroyInstance(instance, nullptr);
  return j.out;
}

} // namespace vk11
