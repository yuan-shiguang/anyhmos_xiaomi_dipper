#include <stdio.h>
/* SPDX-License-Identifier: MIT
 * Real linear DMA-BUF import and explicit sync for the Ace5 OHOS port.
 * Only the formats/layouts exported by our allocator are supported.
 */
#include "tu_device.h"
#include "tu_image.h"
#include "tu_entrypoints.h"
#include "vk_queue.h"
#include "vk_util.h"
#include "drm-uapi/drm_fourcc.h"
#include "vk_alloc.h"
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

static VkFormat ohos_format(int format) {
 switch(format) {
 case 3: return VK_FORMAT_R5G6B5_UNORM_PACK16;
 case 11: case 12: return VK_FORMAT_R8G8B8A8_UNORM;
 case 19: case 20: return VK_FORMAT_B8G8R8A8_UNORM;
 case 28: case 29: return VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
 case 24: return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; // NV12, linear Y then interleaved UV
 default: return VK_FORMAT_UNDEFINED;
 }
}
bool tu_ohos_supported_format(VkFormat f) {
 return f==VK_FORMAT_R5G6B5_UNORM_PACK16 || f==VK_FORMAT_R8G8B8A8_UNORM || f==VK_FORMAT_B8G8R8A8_UNORM || f==VK_FORMAT_G8_B8R8_2PLANE_420_UNORM || f==VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
}
// BLOB width/stride are byte counts, not image dimensions. Never admit this
// format through image validation or advertise image sampling/render features.
bool tu_ohos_valid_blob_handle(const Ace5OHBufferHandle *h) {
 if(!h || h->fd<0 || h->format!=38 || h->width<=0 || h->width>256*1024*1024 ||
    h->height!=1 || h->stride!=h->width || h->size<=0 || h->reserveFds || h->reserveInts ||
    (h->usage & ((1ULL<<11)|(7ULL<<8)))) return false;
 uint64_t page_size=(uint64_t(h->width)+4095)&~uint64_t(4095);
 return uint64_t(h->size)==page_size;
}
bool tu_ohos_valid_handle(const Ace5OHBufferHandle *h) {
 if(!h || h->fd<0 || h->width<=0 || h->height<=0 || h->width>16384 || h->height>16384 ||
    h->size<=0 || h->reserveFds || h->reserveInts || (h->usage & (1ULL<<11))) return false;
 bool yuv=h->format==24 || h->format==28 || h->format==29;
 if(yuv && ((h->width | h->height)&1))return false;
 int bpp=yuv?1:h->format==3?2:4;
 uint64_t required=uint64_t(h->stride)*h->height;
 if(yuv)required+=required/2;
 return ohos_format(h->format)!=VK_FORMAT_UNDEFINED && h->stride>=h->width*bpp &&
    !(h->stride%64) && required<=uint64_t(h->size);
}
VkResult tu_ohos_bind_layout(tu_device *device,tu_image *image,const Ace5OHBufferHandle *h) {
 bool valid=tu_ohos_valid_handle(h);
 fprintf(stderr,"DIPPER_BIND valid=%d vkformat=%u nativeformat=%d vkextent=%ux%u native=%dx%d stride=%d size=%d samples=%u flags=%x\n",valid,image->vk.format,h?h->format:-1,image->vk.extent.width,image->vk.extent.height,h?h->width:-1,h?h->height:-1,h?h->stride:-1,h?h->size:-1,image->vk.samples,image->vk.create_flags);
 if(!valid || image->vk.format!=ohos_format(h->format) ||
    image->vk.extent.width!=unsigned(h->width) || image->vk.extent.height!=unsigned(h->height) ||
    image->vk.samples!=VK_SAMPLE_COUNT_1_BIT || image->vk.create_flags&VK_IMAGE_CREATE_PROTECTED_BIT)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
 VkSubresourceLayout planes[3]={};planes[0].rowPitch=h->stride;planes[0].size=uint64_t(h->stride)*h->height;
 if(h->format==24){planes[1].offset=planes[0].size;planes[1].rowPitch=h->stride;planes[1].size=planes[0].size/2;}
 if(h->format==28 || h->format==29) {
  const uint64_t chromaSize=uint64_t(h->stride/2)*(h->height/2);
  planes[1].offset=planes[0].size+(h->format==29?chromaSize:0);
  planes[2].offset=planes[0].size+(h->format==28?chromaSize:0);
  planes[1].rowPitch=planes[2].rowPitch=h->stride/2;
  planes[1].size=planes[2].size=chromaSize;
 }
 VkResult result=TU_CALLX(device,tu_image_update_layout)(device,image,DRM_FORMAT_MOD_LINEAR,planes);
 fprintf(stderr,"DIPPER_BIND layout_result=%d total_size=%llu native_size=%d\n",result,(unsigned long long)image->total_size,h->size);
 if(result!=VK_SUCCESS)return result;
 return image->total_size<=unsigned(h->size)?VK_SUCCESS:VK_ERROR_INVALID_EXTERNAL_HANDLE;
}
VkResult tu_ohos_import_image(tu_device *device,tu_image *image,const VkNativeBufferOHOS *native,const VkAllocationCallbacks *alloc) {
 auto *h=reinterpret_cast<const Ace5OHBufferHandle*>(native->handle);
 // BindImageMemory2 computes the explicit layout once, after importing memory.
 // fdl6_layout accumulates slice sizes; computing it here too doubles the size.
 if(!tu_ohos_valid_handle(h))return VK_ERROR_INVALID_EXTERNAL_HANDLE;
 VkResult r;
 int fd=dup(h->fd);if(fd<0)return VK_ERROR_OUT_OF_HOST_MEMORY;
 VkMemoryDedicatedAllocateInfo dedicated={VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
 dedicated.image=tu_image_to_handle(image);
 VkImportMemoryFdInfoKHR imported={VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
 imported.pNext=&dedicated;imported.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;imported.fd=fd;
 VkMemoryAllocateInfo info={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
 info.pNext=&imported;info.allocationSize=h->size;info.memoryTypeIndex=0;
 r=tu_AllocateMemory(tu_device_to_handle(device),&info,alloc,&image->vk.anb_memory);
 if(r!=VK_SUCCESS){close(fd);return r;}
 auto *memory=tu_device_memory_from_handle(image->vk.anb_memory);
 memory->ohos_handle=*h;memory->ohos_import=true;
 VkBindImageMemoryInfo bind={VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};bind.image=tu_image_to_handle(image);bind.memory=image->vk.anb_memory;
 return tu_BindImageMemory2(tu_device_to_handle(device),1,&bind);
}
VKAPI_ATTR VkResult VKAPI_CALL dipper_observed_tu_GetNativeBufferPropertiesOHOS(VkDevice dev,const OH_NativeBuffer *buffer,VkNativeBufferPropertiesOHOS *out) {
 VK_FROM_HANDLE(tu_device,device,dev);
 auto *h=OH_NativeBuffer_GetBufferHandle(buffer);
 if(!out || (!tu_ohos_valid_handle(h) && !tu_ohos_valid_blob_handle(h))) {
  static unsigned invalid_count=0;
  if(invalid_count++<48)fprintf(stderr,"DIPPER_NATIVE_INVALID handle=%p fd=%d native=%dx%d format=%d stride=%d size=%d usage=%llx reserve=%u/%u out=%p\n",(const void*)h,h?h->fd:-1,h?h->width:-1,h?h->height:-1,h?h->format:-1,h?h->stride:-1,h?h->size:-1,(unsigned long long)(h?h->usage:0),h?h->reserveFds:0,h?h->reserveInts:0,(void*)out);
  return VK_ERROR_INVALID_EXTERNAL_HANDLE;
 }
 out->allocationSize=h->size;out->memoryTypeBits=0;
 for(unsigned i=0;i<device->physical_device->memory.type_count;i++)
  if(device->physical_device->memory.types[i]&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)out->memoryTypeBits|=1u<<i;
 if(!out->memoryTypeBits)return VK_ERROR_INVALID_EXTERNAL_HANDLE;
 auto *format=vk_find_struct(out->pNext,NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS);
 if(format) {
  auto *f=(VkNativeBufferFormatPropertiesOHOS*)format;
  if(h->format==38) {
   f->format=VK_FORMAT_UNDEFINED;f->externalFormat=0;f->formatFeatures=0;
   f->samplerYcbcrConversionComponents={VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
   f->suggestedYcbcrModel=VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
   f->suggestedYcbcrRange=VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
   f->suggestedXChromaOffset=f->suggestedYChromaOffset=VK_CHROMA_LOCATION_COSITED_EVEN;
   return VK_SUCCESS;
  }
  // Rosen/Skia imports NV12 through its external YCbCr texture path.
  // Opaque ID 24 is the OHOS linear NV12 native-buffer format, not a VkFormat.
  const bool yuv=h->format==24 || h->format==28 || h->format==29;
  f->format=yuv?VK_FORMAT_UNDEFINED:ohos_format(h->format);
  f->externalFormat=yuv?h->format:0;
  VkFormatProperties2 props={VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
  tu_GetPhysicalDeviceFormatProperties2(tu_physical_device_to_handle(device->physical_device),ohos_format(h->format),&props);
  f->formatFeatures=props.formatProperties.linearTilingFeatures;
  f->samplerYcbcrConversionComponents={VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
  f->suggestedYcbcrModel=yuv?VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601:VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
  f->suggestedYcbcrRange=yuv?VK_SAMPLER_YCBCR_RANGE_ITU_NARROW:VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
  f->suggestedXChromaOffset=f->suggestedYChromaOffset=VK_CHROMA_LOCATION_COSITED_EVEN;
 }
 return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL tu_GetMemoryNativeBufferOHOS(VkDevice,const VkMemoryGetNativeBufferInfoOHOS *info,OH_NativeBuffer **out) {
 if(!out)return VK_ERROR_INVALID_EXTERNAL_HANDLE;*out=nullptr;
 if(!info)return VK_ERROR_INVALID_EXTERNAL_HANDLE;
 VK_FROM_HANDLE(tu_device_memory,mem,info->memory);
 if(!mem || !mem->ohos_buffer)return VK_ERROR_INVALID_EXTERNAL_HANDLE;
 if(OH_NativeBuffer_Reference(mem->ohos_buffer)!=0)return VK_ERROR_INVALID_EXTERNAL_HANDLE;
 *out=mem->ohos_buffer;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL tu_GetSwapchainGrallocUsageOHOS(VkDevice,VkFormat format,VkImageUsageFlags usage,uint64_t *out) {
 if(!out || !tu_ohos_supported_format(format))return VK_ERROR_FORMAT_NOT_SUPPORTED;
 const VkImageUsageFlags supported=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
 if(usage&~supported)return VK_ERROR_FORMAT_NOT_SUPPORTED;
 *out=3|(1ULL<<3)|(1ULL<<8)|(1ULL<<9)|(1ULL<<10);return VK_SUCCESS;
}

/* Sync implementation adapted from Mesa vk_android.c (MIT). */
VKAPI_ATTR VkResult VKAPI_CALL
tu_AcquireImageOHOS(VkDevice _device,
                              VkImage image,
                              int nativeFenceFd,
                              VkSemaphore semaphore,
                              VkFence fence)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VkResult result = VK_SUCCESS;

   /* From https://source.android.com/devices/graphics/implement-vulkan :
    *
    *    "The driver takes ownership of the fence file descriptor and closes
    *    the fence file descriptor when no longer needed. The driver must do
    *    so even if neither a semaphore or fence object is provided, or even
    *    if vkAcquireImageANDROID fails and returns an error."
    *
    * The Vulkan spec for VkImportFence/SemaphoreFdKHR(), however, requires
    * the file descriptor to be left alone on failure.
    */
   int semaphore_fd = -1, fence_fd = -1;
   if (nativeFenceFd >= 0) {
      if (semaphore != VK_NULL_HANDLE && fence != VK_NULL_HANDLE) {
         /* We have both so we have to import the sync file twice. One of
          * them needs to be a dup.
          */
         semaphore_fd = nativeFenceFd;
         fence_fd = dup(nativeFenceFd);
         if (fence_fd < 0) {
            VkResult err = (errno == EMFILE) ? VK_ERROR_TOO_MANY_OBJECTS :
                                               VK_ERROR_OUT_OF_HOST_MEMORY;
            close(nativeFenceFd);
            return vk_error(device, err);
         }
      } else if (semaphore != VK_NULL_HANDLE) {
         semaphore_fd = nativeFenceFd;
      } else if (fence != VK_NULL_HANDLE) {
         fence_fd = nativeFenceFd;
      } else {
         /* Nothing to import into so we have to close the file */
         close(nativeFenceFd);
      }
   }

   if (semaphore != VK_NULL_HANDLE) {
      const VkImportSemaphoreFdInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
         .semaphore = semaphore,
         .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = semaphore_fd,
      };
      result = device->dispatch_table.ImportSemaphoreFdKHR(_device, &info);
      if (result == VK_SUCCESS)
         semaphore_fd = -1; /* The driver took ownership */
   }

   if (result == VK_SUCCESS && fence != VK_NULL_HANDLE) {
      const VkImportFenceFdInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
         .fence = fence,
         .flags = VK_FENCE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = fence_fd,
      };
      result = device->dispatch_table.ImportFenceFdKHR(_device, &info);
      if (result == VK_SUCCESS)
         fence_fd = -1; /* The driver took ownership */
   }

   if (semaphore_fd >= 0)
      close(semaphore_fd);
   if (fence_fd >= 0)
      close(fence_fd);

   return result;
}

static VkResult
vk_anb_semaphore_init_once(struct vk_queue *queue, struct vk_device *device)
{
   if (queue->anb_semaphore != VK_NULL_HANDLE)
      return VK_SUCCESS;

   const VkExportSemaphoreCreateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   const VkSemaphoreCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &export_info,
   };
   return device->dispatch_table.CreateSemaphore(vk_device_to_handle(device),
                                                 &create_info, NULL,
                                                 &queue->anb_semaphore);
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_QueueSignalReleaseImageOHOS(VkQueue _queue,
                                         uint32_t waitSemaphoreCount,
                                         const VkSemaphore *pWaitSemaphores,
                                         VkImage image,
                                         int *pNativeFenceFd)
{
   VK_FROM_HANDLE(vk_queue, queue, _queue);
   struct vk_device *device = queue->base.device;
   VkResult result = VK_SUCCESS;

   STACK_ARRAY(VkPipelineStageFlags, stage_flags, MAX2(1, waitSemaphoreCount));
   for (uint32_t i = 0; i < MAX2(1, waitSemaphoreCount); i++)
      stage_flags[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

   result = vk_anb_semaphore_init_once(queue, device);
   if (result != VK_SUCCESS) {
      STACK_ARRAY_FINISH(stage_flags);
      return result;
   }

   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = waitSemaphoreCount,
      .pWaitSemaphores = pWaitSemaphores,
      .pWaitDstStageMask = stage_flags,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &queue->anb_semaphore,
   };
   result = device->dispatch_table.QueueSubmit(_queue, 1, &submit_info,
                                               VK_NULL_HANDLE);
   STACK_ARRAY_FINISH(stage_flags);
   if (result != VK_SUCCESS)
      return result;

   const VkSemaphoreGetFdInfoKHR get_fd = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      .semaphore = queue->anb_semaphore,
   };
   return device->dispatch_table.GetSemaphoreFdKHR(vk_device_to_handle(device),
                                                   &get_fd, pNativeFenceFd);
}


VKAPI_ATTR VkResult VKAPI_CALL
tu_GetNativeBufferPropertiesOHOS(VkDevice device, const OH_NativeBuffer *buffer,
                                 VkNativeBufferPropertiesOHOS *out)
{
   VkResult result = dipper_observed_tu_GetNativeBufferPropertiesOHOS(device, buffer, out);
   static unsigned calls = 0, failures = 0;
   unsigned n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
   if (n <= 12 || (result != VK_SUCCESS && __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED) <= 32))
      fprintf(stderr, "DIPPER_PROPERTIES n=%u result=%d native=%p size=%llu types=%x\n",
              n, result, (const void*)buffer, out ? (unsigned long long)out->allocationSize : 0,
              out ? out->memoryTypeBits : 0);
   return result;
}
