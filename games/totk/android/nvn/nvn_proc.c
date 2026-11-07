/* NVN module: proc. */
#include "nvn_internal.h"

static const nvn_entry nvn_entries[] = {
    NVN_E(nvnDeviceBuilderSetDefaults), NVN_E(nvnDeviceBuilderSetFlags),
    NVN_E(nvnDeviceBuilderGetFlags), NVN_E(nvnDeviceInitialize),
    NVN_E(nvnDeviceFinalize), NVN_E(nvnDeviceSetDebugLabel),
    NVN_E(nvnDeviceGetInteger), NVN_E(nvnDeviceGetCurrentTimestampInNanoseconds),
    NVN_E(nvnDeviceGetTimestampInNanoseconds), NVN_E(nvnDeviceSetIntermediateShaderCache),
    NVN_E(nvnDeviceGetTextureHandle), NVN_E(nvnDeviceGetTexelFetchHandle),
    NVN_E(nvnDeviceGetImageHandle), NVN_E(nvnDeviceInstallDebugCallback),
    NVN_E(nvnDeviceGenerateDebugDomainId), NVN_E(nvnDeviceSetWindowOriginMode),
    NVN_E(nvnDeviceSetDepthMode), NVN_E(nvnDeviceRegisterFastClearColor),
    NVN_E(nvnDeviceRegisterFastClearColori), NVN_E(nvnDeviceRegisterFastClearColorui),
    NVN_E(nvnDeviceRegisterFastClearDepth), NVN_E(nvnDeviceGetWindowOriginMode),
    NVN_E(nvnDeviceGetDepthMode), NVN_E(nvnDeviceApplyDeferredFinalizes),
    NVN_E(nvnDeviceFinalizeCommandHandle), NVN_E(nvnDeviceWalkDebugDatabase),
    NVN_E(nvnDeviceGetSeparateTextureHandle), NVN_E(nvnDeviceGetSeparateSamplerHandle),
    NVN_E(nvnDeviceIsExternalDebuggerAttached), NVN_E(nvnDeviceSetAutomaticCheckpointsEnable),
    NVN_E(nvnDeviceSetAutomaticCheckpointsMask), NVN_E(nvnDeviceSetAutomaticCheckpointsSamplingInterval),
    NVN_E(nvnDeviceSetAutomaticCheckpointsByDebugGroup), NVN_E(nvnDeviceSetCheckpointCallStacksEnable),
    NVN_E(nvnDeviceSetCheckpointMaxCallStacksDepth),

    NVN_E(nvnQueueBuilderSetDefaults), NVN_E(nvnQueueBuilderSetDevice),
    NVN_E(nvnQueueBuilderSetFlags), NVN_E(nvnQueueBuilderSetCommandMemorySize),
    NVN_E(nvnQueueBuilderSetComputeMemorySize), NVN_E(nvnQueueBuilderSetControlMemorySize),
    NVN_E(nvnQueueBuilderSetCommandFlushThreshold), NVN_E(nvnQueueBuilderSetQueuePriority),
    NVN_E(nvnQueueBuilderGetQueuePriority), NVN_E(nvnQueueBuilderGetQueueMemorySize),
    NVN_E(nvnQueueBuilderSetQueueMemory), NVN_E(nvnQueueBuilderGetMemory),
    NVN_E(nvnQueueBuilderGetMemorySize), NVN_E(nvnQueueBuilderGetDevice),
    NVN_E(nvnQueueBuilderGetFlags), NVN_E(nvnQueueBuilderGetCommandMemorySize),
    NVN_E(nvnQueueBuilderGetComputeMemorySize), NVN_E(nvnQueueBuilderGetControlMemorySize),
    NVN_E(nvnQueueBuilderGetCommandFlushThreshold), NVN_E(nvnQueueInitialize),
    NVN_E(nvnQueueFinalize), NVN_E(nvnQueueSetDebugLabel), NVN_E(nvnQueueSubmitCommands),
    NVN_E(nvnQueueFlush), NVN_E(nvnQueueFinish), NVN_E(nvnQueuePresentTexture),
    NVN_E(nvnQueueAcquireTexture), NVN_E(nvnQueueFenceSync), NVN_E(nvnQueueWaitSync),
    NVN_E(nvnQueueGetError), NVN_E(nvnQueueGetTotalCommandMemoryUsed),
    NVN_E(nvnQueueGetTotalControlMemoryUsed), NVN_E(nvnQueueGetTotalComputeMemoryUsed),
    NVN_E(nvnQueueResetMemoryUsageCounts),

    NVN_E(nvnWindowBuilderSetDefaults), NVN_E(nvnWindowBuilderSetDevice),
    NVN_E(nvnWindowBuilderSetNativeWindow), NVN_E(nvnWindowBuilderSetTextures),
    NVN_E(nvnWindowBuilderSetPresentInterval), NVN_E(nvnWindowBuilderSetNumActiveTextures),
    NVN_E(nvnWindowBuilderGetNativeWindow), NVN_E(nvnWindowBuilderGetPresentInterval),
    NVN_E(nvnWindowBuilderGetNumTextures), NVN_E(nvnWindowBuilderGetTexture),
    NVN_E(nvnWindowBuilderGetDevice), NVN_E(nvnWindowBuilderGetNumActiveTextures),
    NVN_E(nvnWindowInitialize), NVN_E(nvnWindowFinalize), NVN_E(nvnWindowSetDebugLabel),
    NVN_E(nvnWindowAcquireTexture), NVN_E(nvnWindowGetNativeWindow),
    NVN_E(nvnWindowGetPresentInterval), NVN_E(nvnWindowSetPresentInterval),
    NVN_E(nvnWindowSetCrop), NVN_E(nvnWindowGetCrop),
    NVN_E(nvnWindowSetNumActiveTextures), NVN_E(nvnWindowGetNumActiveTextures),
    NVN_E(nvnWindowGetNumTextures),

    NVN_E(nvnMemoryPoolBuilderSetDefaults), NVN_E(nvnMemoryPoolBuilderSetDevice),
    NVN_E(nvnMemoryPoolBuilderSetStorage), NVN_E(nvnMemoryPoolBuilderSetFlags),
    NVN_E(nvnMemoryPoolBuilderGetMemory), NVN_E(nvnMemoryPoolBuilderGetSize),
    NVN_E(nvnMemoryPoolBuilderGetFlags), NVN_E(nvnMemoryPoolBuilderGetDevice),
    NVN_E(nvnMemoryPoolInitialize), NVN_E(nvnMemoryPoolFinalize),
    NVN_E(nvnMemoryPoolSetDebugLabel), NVN_E(nvnMemoryPoolMap),
    NVN_E(nvnMemoryPoolFlushMappedRange), NVN_E(nvnMemoryPoolInvalidateMappedRange),
    NVN_E(nvnMemoryPoolGetBufferAddress), NVN_E(nvnMemoryPoolMapVirtual),
    NVN_E(nvnMemoryPoolGetSize), NVN_E(nvnMemoryPoolGetFlags),

    NVN_E(nvnBufferBuilderSetDefaults), NVN_E(nvnBufferBuilderSetDevice),
    NVN_E(nvnBufferBuilderSetStorage), NVN_E(nvnBufferBuilderGetDevice),
    NVN_E(nvnBufferBuilderGetMemoryPool), NVN_E(nvnBufferBuilderGetMemoryOffset),
    NVN_E(nvnBufferBuilderGetSize), NVN_E(nvnBufferInitialize), NVN_E(nvnBufferFinalize),
    NVN_E(nvnBufferSetDebugLabel), NVN_E(nvnBufferMap), NVN_E(nvnBufferGetAddress),
    NVN_E(nvnBufferFlushMappedRange), NVN_E(nvnBufferInvalidateMappedRange),
    NVN_E(nvnBufferGetMemoryPool), NVN_E(nvnBufferGetMemoryOffset),
    NVN_E(nvnBufferGetSize), NVN_E(nvnBufferGetDebugID),

    NVN_E(nvnTextureBuilderSetDefaults), NVN_E(nvnTextureBuilderSetDevice),
    NVN_E(nvnTextureBuilderSetFlags), NVN_E(nvnTextureBuilderSetTarget),
    NVN_E(nvnTextureBuilderSetWidth), NVN_E(nvnTextureBuilderSetHeight),
    NVN_E(nvnTextureBuilderSetDepth), NVN_E(nvnTextureBuilderSetSize1D),
    NVN_E(nvnTextureBuilderSetSize2D), NVN_E(nvnTextureBuilderSetSize3D),
    NVN_E(nvnTextureBuilderSetLevels), NVN_E(nvnTextureBuilderSetFormat),
    NVN_E(nvnTextureBuilderSetSamples), NVN_E(nvnTextureBuilderSetSwizzle),
    NVN_E(nvnTextureBuilderSetDepthStencilMode), NVN_E(nvnTextureBuilderSetStorage),
    NVN_E(nvnTextureBuilderSetPackagedTextureData), NVN_E(nvnTextureBuilderSetPackagedTextureLayout),
    NVN_E(nvnTextureBuilderSetStride), NVN_E(nvnTextureBuilderSetGLTextureName),
    NVN_E(nvnTextureBuilderGetStorageSize), NVN_E(nvnTextureBuilderGetStorageAlignment),
    NVN_E(nvnTextureBuilderGetStorageClass), NVN_E(nvnTextureBuilderGetViewOffset),
    NVN_E(nvnTextureBuilderGetPackagedTextureLayout), NVN_E(nvnTextureBuilderGetRawStorageClass),
    NVN_E(nvnTextureBuilderGetFlags),
    NVN_E(nvnTextureBuilderGetTarget), NVN_E(nvnTextureBuilderGetWidth),
    NVN_E(nvnTextureBuilderGetHeight), NVN_E(nvnTextureBuilderGetDepth),
    NVN_E(nvnTextureBuilderGetLevels), NVN_E(nvnTextureBuilderGetFormat),
    NVN_E(nvnTextureBuilderGetSamples), NVN_E(nvnTextureBuilderGetSwizzle),
    NVN_E(nvnTextureBuilderGetDepthStencilMode), NVN_E(nvnTextureBuilderGetPackagedTextureData),
    NVN_E(nvnTextureBuilderGetStride), NVN_E(nvnTextureBuilderGetSparseTileLayout),
    NVN_E(nvnTextureBuilderGetGLTextureName), NVN_E(nvnTextureBuilderGetZCullStorageSize),
    NVN_E(nvnTextureBuilderGetMemoryPool), NVN_E(nvnTextureBuilderGetMemoryOffset),
    NVN_E(nvnTextureBuilderGetDevice),
    NVN_E(nvnTextureInitialize), NVN_E(nvnTextureFinalize), NVN_E(nvnTextureSetDebugLabel),
    NVN_E(nvnTextureGetStorageClass), NVN_E(nvnTextureGetViewOffset), NVN_E(nvnTextureGetFlags),
    NVN_E(nvnTextureGetTarget), NVN_E(nvnTextureGetWidth), NVN_E(nvnTextureGetHeight),
    NVN_E(nvnTextureGetDepth), NVN_E(nvnTextureGetLevels), NVN_E(nvnTextureGetFormat),
    NVN_E(nvnTextureGetSamples), NVN_E(nvnTextureGetSwizzle), NVN_E(nvnTextureGetDepthStencilMode),
    NVN_E(nvnTextureGetStride), NVN_E(nvnTextureGetTextureAddress),
    NVN_E(nvnTextureGetSparseTileLayout), NVN_E(nvnTextureWriteTexels),
    NVN_E(nvnTextureWriteTexelsStrided), NVN_E(nvnTextureReadTexels),
    NVN_E(nvnTextureReadTexelsStrided), NVN_E(nvnTextureFlushTexels),
    NVN_E(nvnTextureInvalidateTexels), NVN_E(nvnTextureGetMemoryPool),
    NVN_E(nvnTextureGetMemoryOffset), NVN_E(nvnTextureGetStorageSize),
    NVN_E(nvnTextureCompare), NVN_E(nvnTextureGetDebugID), NVN_E(nvnTextureGetRawStorageClass),
    NVN_E(nvnTextureGetZCullStorageSize),

    NVN_E(nvnTextureViewSetDefaults), NVN_E(nvnTextureViewSetLevels),
    NVN_E(nvnTextureViewSetLayers), NVN_E(nvnTextureViewSetFormat),
    NVN_E(nvnTextureViewSetSwizzle), NVN_E(nvnTextureViewSetDepthStencilMode),
    NVN_E(nvnTextureViewSetTarget), NVN_E(nvnTextureViewGetLevels),
    NVN_E(nvnTextureViewGetLayers), NVN_E(nvnTextureViewGetFormat),
    NVN_E(nvnTextureViewGetSwizzle), NVN_E(nvnTextureViewGetDepthStencilMode),
    NVN_E(nvnTextureViewGetTarget), NVN_E(nvnTextureViewCompare),

    NVN_E(nvnSamplerBuilderSetDefaults), NVN_E(nvnSamplerBuilderSetDevice),
    NVN_E(nvnSamplerBuilderSetMinMagFilter), NVN_E(nvnSamplerBuilderSetWrapMode),
    NVN_E(nvnSamplerBuilderSetLodClamp), NVN_E(nvnSamplerBuilderSetLodBias),
    NVN_E(nvnSamplerBuilderSetCompare), NVN_E(nvnSamplerBuilderSetBorderColor),
    NVN_E(nvnSamplerBuilderSetBorderColori), NVN_E(nvnSamplerBuilderSetBorderColorui),
    NVN_E(nvnSamplerBuilderSetMaxAnisotropy), NVN_E(nvnSamplerBuilderSetReductionFilter),
    NVN_E(nvnSamplerBuilderSetLodSnap), NVN_E(nvnSamplerBuilderGetDevice),
    NVN_E(nvnSamplerBuilderGetMinMagFilter), NVN_E(nvnSamplerBuilderGetWrapMode),
    NVN_E(nvnSamplerBuilderGetLodClamp), NVN_E(nvnSamplerBuilderGetLodBias),
    NVN_E(nvnSamplerBuilderGetCompare), NVN_E(nvnSamplerBuilderGetBorderColor),
    NVN_E(nvnSamplerBuilderGetBorderColori), NVN_E(nvnSamplerBuilderGetBorderColorui),
    NVN_E(nvnSamplerBuilderGetMaxAnisotropy), NVN_E(nvnSamplerBuilderGetReductionFilter),
    NVN_E(nvnSamplerBuilderGetLodSnap),
    NVN_E(nvnSamplerInitialize), NVN_E(nvnSamplerFinalize), NVN_E(nvnSamplerSetDebugLabel),
    NVN_E(nvnSamplerGetMinMagFilter), NVN_E(nvnSamplerGetWrapMode),
    NVN_E(nvnSamplerGetLodClamp), NVN_E(nvnSamplerGetLodBias),
    NVN_E(nvnSamplerGetCompare), NVN_E(nvnSamplerGetBorderColor),
    NVN_E(nvnSamplerGetBorderColori), NVN_E(nvnSamplerGetBorderColorui),
    NVN_E(nvnSamplerGetMaxAnisotropy), NVN_E(nvnSamplerGetReductionFilter),
    NVN_E(nvnSamplerCompare), NVN_E(nvnSamplerGetDebugID),

    NVN_E(nvnTexturePoolInitialize), NVN_E(nvnTexturePoolFinalize),
    NVN_E(nvnTexturePoolSetDebugLabel), NVN_E(nvnTexturePoolRegisterTexture),
    NVN_E(nvnTexturePoolRegisterImage), NVN_E(nvnTexturePoolGetMemoryPool),
    NVN_E(nvnTexturePoolGetMemoryOffset), NVN_E(nvnTexturePoolGetSize),
    NVN_E(nvnSamplerPoolInitialize), NVN_E(nvnSamplerPoolFinalize),
    NVN_E(nvnSamplerPoolSetDebugLabel), NVN_E(nvnSamplerPoolRegisterSampler),
    NVN_E(nvnSamplerPoolRegisterSamplerBuilder), NVN_E(nvnSamplerPoolGetMemoryPool),
    NVN_E(nvnSamplerPoolGetMemoryOffset), NVN_E(nvnSamplerPoolGetSize),

    NVN_E(nvnCommandBufferInitialize), NVN_E(nvnCommandBufferFinalize),
    NVN_E(nvnCommandBufferSetDebugLabel), NVN_E(nvnCommandBufferSetMemoryCallback),
    NVN_E(nvnCommandBufferSetMemoryCallbackData), NVN_E(nvnCommandBufferSetControlMemoryCallbackEnabled),
    NVN_E(nvnCommandBufferSetCommandMemoryCallbackEnabled), NVN_E(nvnCommandBufferAddCommandMemory),
    NVN_E(nvnCommandBufferAddControlMemory), NVN_E(nvnCommandBufferGetControlMemorySize),
    NVN_E(nvnCommandBufferGetControlMemoryUsed), NVN_E(nvnCommandBufferGetCommandMemorySize),
    NVN_E(nvnCommandBufferGetCommandMemoryUsed), NVN_E(nvnCommandBufferBeginRecording),
    NVN_E(nvnCommandBufferEndRecording), NVN_E(nvnCommandBufferSetRenderEnable),
    NVN_E(nvn_trace_dispatch_compute), NVN_E(nvn_trace_dispatch_indirect),
    NVN_E(nvn_trace_draw_instanced), NVN_E(nvn_trace_draw_indirect),
    NVN_E(nvn_trace_draw_arrays_indirect), NVN_E(nvn_trace_draw_texture),
    NVN_E(nvn_trace_clear_texture), NVN_E(nvn_trace_clear_texturei),
    NVN_E(nvn_trace_clear_textureui), NVN_E(nvn_trace_copy_b2t),
    NVN_E(nvn_trace_copy_t2b), NVN_E(nvn_trace_barrier),
    NVN_E(nvn_trace_cb_fence), NVN_E(nvn_trace_cb_wait_sync),
    NVN_E(nvn_trace_cb_signal_event), NVN_E(nvn_trace_cb_wait_event),
    NVN_E(nvn_trace_render_enable_cond),
    NVN_E(nvnCommandBufferSetRenderTargets), NVN_E(nvnCommandBufferClearColor),
    NVN_E(nvnCommandBufferClearColori), NVN_E(nvnCommandBufferClearColorui),
    NVN_E(nvnCommandBufferDrawArrays), NVN_E(nvnCommandBufferDrawArraysInstanced),
    NVN_E(nvnCommandBufferDrawElements), NVN_E(nvnCommandBufferDrawElementsBaseVertex), NVN_E(nvnCommandBufferClearDepthStencil),
    NVN_E(nvnCommandBufferSetViewport), NVN_E(nvnCommandBufferSetViewports),
    NVN_E(nvnCommandBufferSetScissor), NVN_E(nvnCommandBufferSetScissors),
    NVN_E(nvnCommandBufferBindProgram), NVN_E(nvnCommandBufferBindVertexBuffer),
    NVN_E(nvnCommandBufferBindVertexBuffers), NVN_E(nvnCommandBufferBindVertexAttribState),
    NVN_E(nvnCommandBufferBindVertexStreamState), NVN_E(nvnCommandBufferSetPrimitiveRestart),
    NVN_E(nvnCommandBufferCopyBufferToBuffer), NVN_E(nvnCommandBufferClearBuffer),
    NVN_E(nvnCommandBufferCallCommands), NVN_E(nvnCommandBufferCopyCommands),
    NVN_E(nvnCommandBufferBindBlendState), NVN_E(nvnCommandBufferBindColorState),
    NVN_E(nvnCommandBufferBindChannelMaskState), NVN_E(nvnCommandBufferBindMultisampleState),
    NVN_E(nvnCommandBufferBindPolygonState), NVN_E(nvnCommandBufferBindDepthStencilState),
    NVN_E(nvnCommandBufferSetBlendColor), NVN_E(nvnCommandBufferSetStencilRef),
    NVN_E(nvnCommandBufferSetStencilMask), NVN_E(nvnCommandBufferSetStencilValueMask),
    NVN_E(nvnCommandBufferSetTexturePool),
    NVN_E(nvnCommandBufferSetSamplerPool), NVN_E(nvnCommandBufferBindUniformBuffer),
    NVN_E(nvnCommandBufferUpdateUniformBuffer),
    NVN_E(nvnCommandBufferBindUniformBuffers), NVN_E(nvnCommandBufferBindStorageBuffer),
    NVN_E(nvnCommandBufferBindStorageBuffers), NVN_E(nvnCommandBufferBindTexture),
    NVN_E(nvnCommandBufferBindTextures), NVN_E(nvnCommandBufferBindImage),
    NVN_E(nvnCommandBufferBindImages),
    NVN_E(nvn_trace_dispatch_compute), NVN_E(nvn_trace_dispatch_indirect),
    NVN_E(nvn_trace_draw_texture), NVN_E(nvn_trace_clear_texture),
    NVN_E(nvn_trace_clear_texturei), NVN_E(nvn_trace_clear_textureui),
    NVN_E(nvn_trace_copy_b2t), NVN_E(nvn_trace_copy_t2b), NVN_E(nvn_trace_copy_t2t),
    NVN_E(nvn_trace_blit),
    NVN_E(nvnBlendStateSetDefaults), NVN_E(nvnBlendStateSetBlendTarget),
    NVN_E(nvnBlendStateSetBlendFunc), NVN_E(nvnBlendStateSetBlendEquation),
    NVN_E(nvnColorStateSetDefaults), NVN_E(nvnColorStateSetBlendEnable),
    NVN_E(nvnChannelMaskStateSetDefaults), NVN_E(nvnChannelMaskStateSetChannelMask),
    NVN_E(nvnMultisampleStateSetDefaults), NVN_E(nvnMultisampleStateSetMultisampleEnable),
    NVN_E(nvnMultisampleStateSetSamples), NVN_E(nvnMultisampleStateSetAlphaToCoverageEnable),
    NVN_E(nvnMultisampleStateSetSampleLocationsEnable),
    NVN_E(nvnMultisampleStateSetSampleLocations),
    NVN_E(nvnPolygonStateSetDefaults), NVN_E(nvnPolygonStateSetCullFace),
    NVN_E(nvnPolygonStateSetFrontFace), NVN_E(nvnPolygonStateSetPolygonMode),
    NVN_E(nvnPolygonStateSetPolygonOffsetEnables),
    NVN_E(nvnDepthStencilStateSetDefaults), NVN_E(nvnDepthStencilStateSetDepthTestEnable),
    NVN_E(nvnDepthStencilStateSetDepthWriteEnable), NVN_E(nvnDepthStencilStateSetDepthFunc),
    NVN_E(nvnDepthStencilStateSetStencilTestEnable), NVN_E(nvnDepthStencilStateSetStencilFunc),
    NVN_E(nvnDepthStencilStateSetStencilOp),
    NVN_E(nvnVertexAttribStateSetDefaults), NVN_E(nvnVertexAttribStateSetFormat),
    NVN_E(nvnVertexAttribStateSetStreamIndex), NVN_E(nvnVertexAttribStateGetFormat),
    NVN_E(nvnVertexAttribStateGetStreamIndex),
    NVN_E(nvnVertexStreamStateSetDefaults), NVN_E(nvnVertexStreamStateSetStride),
    NVN_E(nvnVertexStreamStateSetDivisor), NVN_E(nvnVertexStreamStateGetStride),
    NVN_E(nvnVertexStreamStateGetDivisor),
    NVN_E(nvnProgramInitialize), NVN_E(nvnProgramFinalize), NVN_E(nvnProgramSetDebugLabel),
    NVN_E(nvnProgramSetShaders), NVN_E(nvnProgramSetShadersExt),
    NVN_E(nvnProgramSetSampleShading), NVN_E(nvnProgramGetSubtileSize),
    NVN_E(nvnProgramSetSubroutineLinkage),

    NVN_E(nvnSyncInitialize), NVN_E(nvnSyncFinalize), NVN_E(nvnSyncSetDebugLabel),
    NVN_E(nvnSyncWait), NVN_E(nvnSyncInitializeFromFencedGLSync), NVN_E(nvnSyncCreateGLSync),
    NVN_E(nvnEventBuilderSetDefaults), NVN_E(nvnEventBuilderSetStorage),
    NVN_E(nvnEventBuilderGetStorage), NVN_E(nvnEventBuilderGetMemoryPool),
    NVN_E(nvnEventBuilderGetMemoryOffset), NVN_E(nvnEventInitialize),
    NVN_E(nvnEventFinalize), NVN_E(nvnEventGetValue), NVN_E(nvnEventSignal),
    NVN_E(nvnEventGetMemoryPool), NVN_E(nvnEventGetMemoryOffset),
};

/* ------------------------------------------------------------------ */
/* Proc-address table                                                  */

/* Trace-only shims for NVN commands that are still unimplemented.  The
 * engine receives a non-NULL entry from nvn_lookup even for a missing proc, so
 * a silent nvn_zero64 hides whether the scene path actually calls it.  These
 * shims log the first call (TRACE_FRAME) and keep the old no-op behaviour. */
void nvn_zero_call(const char *name) {
    if (nvn_trace_composite())
        fprintf(stderr, "[totk-frame] composite call %s (unimplemented no-op)\n", name);
    static char seen[32][48];
    static unsigned n;
    for (unsigned i = 0; i < n; i++) if (!strcmp(seen[i], name)) return;
    if (n < sizeof seen / sizeof seen[0]) snprintf(seen[n++], sizeof seen[0], "%s", name);
    if (nvn_trace_frame()) fprintf(stderr, "[totk-frame] call %s (unimplemented no-op)\n", name);
}

/* NVNtextureView (16 bytes): flag byte 0 selects which fields are present. */
void nvn_view_level_layer(const void *view, uint32_t *level, uint32_t *layer) {
    const uint8_t *v = view;
    *level = 0;
    *layer = 0;
    if (!v) return;
    if (v[0] & 1) *level = v[1];
    if (v[0] & 2) *layer = (uint32_t)v[4] | ((uint32_t)v[5] << 8);
}

#undef NVN_E

uint64_t nvn_zero64(void) { return 0; }

void *nvn_lookup(const char *name) {
    if (!name) return NULL;
    snprintf(g_last_proc, sizeof g_last_proc, "%s", name);
    if (nvn_trace_composite() &&
        (strstr(name, "DrawTexture") || strstr(name, "ClearTexture") ||
         strstr(name, "CopyTexture") || strstr(name, "Blit") ||
         strstr(name, "Dispatch")))
        fprintf(stderr, "[totk-frame] composite proc lookup %s\n", name);
    if (!strcmp(name, "nvnCommandBufferCopyTextureToTexture"))
        return (void *)nvn_trace_copy_t2t;
    if (!strcmp(name, "nvnCommandBufferBlit"))
        return (void *)nvn_trace_blit;
    for (unsigned i = 0; i < sizeof nvn_entries / sizeof *nvn_entries; i++)
        if (!strcmp(name, nvn_entries[i].name)) return (void *)nvn_entries[i].fn;
    static unsigned logged;
    if (nvn_trace() || (nvn_trace_first() && logged++ < 128))
        fprintf(stderr, "[totk-nvn] proc lookup %s (unimplemented)\n", name);
    /* Object APIs must not silently report success.  The object categories
     * below have complete implementations, so an unknown name here is a real
     * gap: report it and return a zero-returning entry. */
    static const char *const object_prefixes[] = {
        "nvnDevice", "nvnQueue", "nvnWindow", "nvnMemoryPool", "nvnBuffer",
        "nvnTexture", "nvnSampler", "nvnProgram", "nvnSync", "nvnEvent",
    };
    for (unsigned i = 0; i < sizeof object_prefixes / sizeof *object_prefixes; i++)
        if (!strncmp(name, object_prefixes[i], strlen(object_prefixes[i])))
            nvn_fail("unimplemented NVN object API", name);
    return (void *)nvn_zero64;
}

void *nvnDeviceGetProcAddress(const nvn_dev_t *d, const char *name) {
    nvn_api("nvnDeviceGetProcAddress");
    (void)d;
    if (!name) return NULL;
    if (!strcmp(name, "nvnDeviceGetProcAddress")) return (void *)nvnDeviceGetProcAddress;
    void *fn = nvn_lookup(name);
    if (nvn_trace_frame()) {
        static char seen[512][56];
        static unsigned nseen;
        for (unsigned i = 0; i < nseen; i++) if (!strcmp(seen[i], name)) return fn;
        if (nseen < sizeof seen / sizeof seen[0]) snprintf(seen[nseen++], sizeof seen[0], "%s", name);
        fprintf(stderr, "[totk-frame] proc %s %s\n", name,
                fn == (void *)nvn_zero64 ? "UNIMPLEMENTED" : "ok");
    }
    return fn;
}

void *runtime_nvn_bootstrap(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "nvnDeviceInitialize")) return (void *)nvnDeviceInitialize;
    if (!strcmp(name, "nvnDeviceGetProcAddress")) return (void *)nvnDeviceGetProcAddress;
    return nvn_lookup(name);
}
