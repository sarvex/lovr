#include "spatializer.h"
#include "core/maf.h"
#include <phonon_version.h>
#include <phonon.h>
#include <stdlib.h>
#include <string.h>

#define PHONON_THREADS 1
#define PHONON_RAYS 4096
#define PHONON_BOUNCES 4
#define PHONON_DIFFUSE_SAMPLES 1024
#define PHONON_OCCLUSION_SAMPLES 32
#define PHONON_MAX_REVERB 1.f
// If this is changed, the scratchpad needs to grow to account for the increase in channels
#define PHONON_AMBISONIC_ORDER 1

#define MONO (IPLAudioFormat) { IPL_CHANNELLAYOUTTYPE_SPEAKERS, IPL_CHANNELLAYOUT_MONO, .channelOrder = IPL_CHANNELORDER_INTERLEAVED }
#define STEREO (IPLAudioFormat) { IPL_CHANNELLAYOUTTYPE_SPEAKERS, IPL_CHANNELLAYOUT_STEREO, .channelOrder = IPL_CHANNELORDER_INTERLEAVED }
#define AMBISONIC (IPLAudioFormat) {\
  .channelLayoutType = IPL_CHANNELLAYOUTTYPE_AMBISONICS,\
  .ambisonicsOrder = PHONON_AMBISONIC_ORDER,\
  .ambisonicsOrdering = IPL_AMBISONICSORDERING_ACN,\
  .ambisonicsNormalization = IPL_AMBISONICSNORMALIZATION_N3D,\
  .channelOrder = IPL_CHANNELORDER_DEINTERLEAVED }

#ifdef _WIN32
#include <windows.h>
#define PHONON_LIBRARY "phonon.dll"
static void* phonon_dlopen(const char* library) { return LoadLibraryA(library); }
static FARPROC phonon_dlsym(void* library, const char* symbol) { return GetProcAddress(library, symbol); }
static void phonon_dlclose(void* library) { FreeLibrary(library); }
#else
#include <dlfcn.h>
#if __APPLE__
#define PHONON_LIBRARY "libphonon.dylib"
#else
#define PHONON_LIBRARY "libphonon.so"
#endif
static void* phonon_dlopen(const char* library) { return dlopen(library, RTLD_NOW | RTLD_LOCAL); }
static void* phonon_dlsym(void* library, const char* symbol) { return dlsym(library, symbol); }
static void phonon_dlclose(void* library) { dlclose(library); }
#endif

typedef IPLerror fn_iplContextCreate(IPLContextSettings* settings, IPLContext* context);
typedef void fn_iplContextRelease(IPLContext* context);
typedef IPLerror fn_iplHRTFCreate(IPLContext context, IPLAudioSettings* audioSettings, IPLHRTFSettings* hrtfSettings, IPLHRTF* hrtf);
typedef void fn_iplHRTFRelease(IPLHRTF* hrtf);
typedef IPLerror fn_iplSceneCreate(IPLContext context, IPLSceneSettings* settings, IPLScene* scene);
typedef void fn_iplSceneRelease(IPLScene* scene);
typedef void fn_iplSceneCommit(IPLScene scene);
typedef IPLerror fn_iplStaticMeshCreate(IPLScene scene, IPLStaticMeshSettings* settings, IPLStaticMesh* staticMesh);
typedef void fn_iplStaticMeshRelease(IPLStaticMesh* staticMesh);
typedef void fn_iplStaticMeshAdd(IPLStaticMesh staticMesh, IPLScene scene);
typedef void fn_iplStaticMeshRemove(IPLStaticMesh staticMesh, IPLScene scene);

#define PHONON_DECLARE(f) static fn_##f* phonon_##f;
#define PHONON_LOAD(f) phonon_##f = (fn_##f*) phonon_dlsym(state.library, #f);
#define PHONON_FOREACH(X)\
  X(iplContextCreate)\
  X(iplContextRelease)\
  X(iplHRTFCreate)\
  X(iplHRTFRelease)\
  X(iplSceneCreate)\
  X(iplSceneRelease)\
  X(iplSceneCommit)\
  X(iplStaticMeshCreate)\
  X(iplStaticMeshRelease)\
  X(iplStaticMeshAdd)\
  X(iplStaticMeshRemove)

PHONON_FOREACH(PHONON_DECLARE)

static struct {
  void* library;
  IPLContext context;
  IPLHRTF hrtf;
  IPLScene scene;
  IPLStaticMesh mesh;
  IPLhandle binauralEffect[MAX_SOURCES];
  IPLhandle directSoundEffect[MAX_SOURCES];
  IPLhandle convolutionEffect[MAX_SOURCES];
  IPLAudioSettings audioSettings;
  float listenerPosition[4];
  float listenerOrientation[4];
  float* scratchpad;
} state;

static void phonon_destroy(void);

bool phonon_init() {
  state.library = phonon_dlopen(PHONON_LIBRARY);
  if (!state.library) return false;

  PHONON_FOREACH(PHONON_LOAD)

  state.audioSettings.samplingRate = lovrAudioGetSampleRate();
  state.audioSettings.frameSize = BUFFER_SIZE;

  IPLerror status;
  IPLContextSettings contextSettings = { .version = STEAMAUDIO_VERSION };
  status = phonon_iplContextCreate(&contextSettings, &state.context);
  if (status != IPL_STATUS_SUCCESS) return phonon_destroy(), false;

  IPLHRTFSettings hrtfSettings = { .type = IPL_HRTFTYPE_DEFAULT };
  status = phonon_iplHRTFCreate(state.context, &state.audioSettings, &hrtfSettings, &state.hrtf);
  if (status != IPL_STATUS_SUCCESS) return phonon_destroy(), false;

  IPLSceneSettings sceneSettings = { .type = IPL_SCENETYPE_DEFAULT };
  status = phonon_iplSceneCreate(state.context, &sceneSettings, &state.scene);
  if (status != IPL_STATUS_SUCCESS) return phonon_destroy(), false;

  state.scratchpad = malloc(BUFFER_SIZE * 4 * sizeof(float));
  if (!state.scratchpad) return phonon_destroy(), false;

  return true;
}

void phonon_destroy() {
  if (state.scratchpad) free(state.scratchpad);
  for (size_t i = 0; i < MAX_SOURCES; i++) {
    if (state.binauralEffect[i]) phonon_iplDestroyBinauralEffect(&state.binauralEffect[i]);
    if (state.directSoundEffect[i]) phonon_iplDestroyDirectSoundEffect(&state.directSoundEffect[i]);
    if (state.convolutionEffect[i]) phonon_iplDestroyConvolutionEffect(&state.convolutionEffect[i]);
  }
  if (state.mesh) phonon_iplStaticMeshRelease(&state.mesh);
  if (state.scene) phonon_iplSceneRelease(&state.scene);
  if (state.hrtf) phonon_iplHRTFRelease(&state.hrtf);
  if (state.context) phonon_iplContextRelease(&state.context);
  phonon_dlclose(state.library);
  memset(&state, 0, sizeof(state));
}

/*
  IPLSimulationSettings settings = (IPLSimulationSettings) {
    .sceneType = IPL_SCENETYPE_PHONON,
    .maxNumOcclusionSamples = PHONON_OCCLUSION_SAMPLES,
    .numRays = PHONON_RAYS,
    .numDiffuseSamples = PHONON_DIFFUSE_SAMPLES,
    .numBounces = PHONON_BOUNCES,
    .numThreads = PHONON_THREADS,
    .irDuration = PHONON_MAX_REVERB,
    .ambisonicsOrder = PHONON_AMBISONIC_ORDER,
    .maxConvolutionSources = MAX_SOURCES,
    .bakingBatchSize = 1,
    .irradianceMinDistance = .1f
  };
*/

uint32_t phonon_apply(Source* source, const float* input, float* output, uint32_t frames, uint32_t why) {
  IPLAudioBuffer in = { .format = MONO, .numSamples = frames, .interleavedBuffer = (float*) input };
  IPLAudioBuffer tmp = { .format = MONO, .numSamples = frames, .interleavedBuffer = state.scratchpad };
  IPLAudioBuffer out = { .format = STEREO, .numSamples = frames, .interleavedBuffer = output };

  uint32_t index = lovrSourceGetIndex(source);

  float x[4], y[4], z[4];
  vec3_set(y, 0.f, 1.f, 0.f);
  vec3_set(z, 0.f, 0.f, -1.f);
  quat_rotate(state.listenerOrientation, y);
  quat_rotate(state.listenerOrientation, z);
  IPLVector3 listener = { state.listenerPosition[0], state.listenerPosition[1], state.listenerPosition[2] };
  IPLVector3 forward = { z[0], z[1], z[2] };
  IPLVector3 up = { y[0], y[1], y[2] };

  // TODO maybe this should use a matrix
  float position[4], orientation[4];
  lovrSourceGetPose(source, position, orientation);
  vec3_set(x, 1.f, 0.f, 0.f);
  vec3_set(y, 0.f, 1.f, 0.f);
  vec3_set(z, 0.f, 0.f, -1.f);
  quat_rotate(orientation, x);
  quat_rotate(orientation, y);
  quat_rotate(orientation, z);

  float weight, power;
  lovrSourceGetDirectivity(source, &weight, &power);

  IPLSource iplSource = {
    .position = (IPLVector3) { position[0], position[1], position[2] },
    .ahead = (IPLVector3) { z[0], z[1], z[2] },
    .up = (IPLVector3) { y[0], y[1], y[2] },
    .right = (IPLVector3) { x[0], x[1], x[2] },
    .airAbsorptionModel.type = IPL_AIRABSORPTION_EXPONENTIAL,
    .directivity.dipoleWeight = weight,
    .directivity.dipolePower = power
  };

  lovrAudioGetAbsorption(iplSource.airAbsorptionModel.coefficients);

  IPLDirectOcclusionMode occlusion = IPL_DIRECTOCCLUSION_NONE;
  IPLDirectOcclusionMethod volumetric = IPL_DIRECTOCCLUSION_RAYCAST;
  float radius = 0.f;
  IPLint32 rays = 0;

  if (state.mesh && lovrSourceIsEffectEnabled(source, EFFECT_OCCLUSION)) {
    bool transmission = lovrSourceIsEffectEnabled(source, EFFECT_TRANSMISSION);
    occlusion = transmission ? IPL_DIRECTOCCLUSION_TRANSMISSIONBYFREQUENCY : IPL_DIRECTOCCLUSION_NOTRANSMISSION;
    radius = lovrSourceGetRadius(source);

    if (radius > 0.f) {
      volumetric = IPL_DIRECTOCCLUSION_VOLUMETRIC;
      rays = PHONON_OCCLUSION_SAMPLES;
    }
  }

  IPLDirectSoundPath path = phonon_iplGetDirectSoundPath(state.environment, listener, forward, up, iplSource, radius, rays, occlusion, volumetric);

  IPLDirectSoundEffectOptions options = {
    .applyDistanceAttenuation = lovrSourceIsEffectEnabled(source, EFFECT_ATTENUATION) ? IPL_TRUE : IPL_FALSE,
    .applyAirAbsorption = lovrSourceIsEffectEnabled(source, EFFECT_ABSORPTION) ? IPL_TRUE : IPL_FALSE,
    .applyDirectivity = weight > 0.f && power > 0.f ? IPL_TRUE : IPL_FALSE,
    .directOcclusionMode = occlusion
  };

  phonon_iplApplyDirectSoundEffect(state.directSoundEffect[index], in, path, options, tmp);

  float blend = 1.f;
  IPLHrtfInterpolation interpolation = IPL_HRTFINTERPOLATION_NEAREST;
  phonon_iplApplyBinauralEffect(state.binauralEffect[index], state.binauralRenderer, tmp, path.direction, interpolation, blend, out);

  if (state.mesh && lovrSourceIsEffectEnabled(source, EFFECT_REVERB)) {
    phonon_iplSetDryAudioForConvolutionEffect(state.convolutionEffect[index], iplSource, in);
  }

  return frames;
}

uint32_t phonon_tail(float* scratch, float* output, uint32_t frames) {
  if (!state.mesh) return 0;

  IPLAudioBuffer out = { .format = STEREO, .numSamples = frames, .interleavedBuffer = output };

  IPLAudioBuffer tmp = {
    .format = AMBISONIC,
    .numSamples = frames,
    .deinterleavedBuffer = (float*[4]) {
      state.scratchpad + frames * 0,
      state.scratchpad + frames * 1,
      state.scratchpad + frames * 2,
      state.scratchpad + frames * 3
    }
  };

  float y[4], z[4];
  vec3_set(y, 0.f, 1.f, 0.f);
  vec3_set(z, 0.f, 0.f, -1.f);
  quat_rotate(state.listenerOrientation, y);
  quat_rotate(state.listenerOrientation, z);
  IPLVector3 listener = { state.listenerPosition[0], state.listenerPosition[1], state.listenerPosition[2] };
  IPLVector3 forward = { z[0], z[1], z[2] };
  IPLVector3 up = { y[0], y[1], y[2] };

  memset(state.scratchpad, 0, 4 * frames * sizeof(float));
  phonon_iplGetMixedEnvironmentalAudio(state.environmentalRenderer, listener, forward, up, tmp);
  phonon_iplApplyAmbisonicsBinauralEffect(state.ambisonicsBinauralEffect, state.binauralRenderer, tmp, out);
  return frames;
}

void phonon_setListenerPose(float position[4], float orientation[4]) {
  memcpy(state.listenerPosition, position, sizeof(state.listenerPosition));
  memcpy(state.listenerOrientation, orientation, sizeof(state.listenerOrientation));
}

// absorption[3], scattering, transmission[3]
static const IPLMaterial materials[] = {
  [MATERIAL_GENERIC] = { { .10f, .20f, .30f }, .05f, { .100f, .050f, .030f } },
  [MATERIAL_BRICK] = { { .03f, .04f, .07f }, .05f, { .015f, .015f, .015f } },
  [MATERIAL_CARPET] = { { .24f, .69f, .73f }, .05f, { .020f, .005f, .003f } },
  [MATERIAL_CERAMIC] = { { .01f, .02f, .02f }, .05f, { .060f, .044f, .011f } },
  [MATERIAL_CONCRETE] = { { .05f, .07f, .08f }, .05f, { .015f, .002f, .001f } },
  [MATERIAL_GLASS] = { { .06f, .03f, .02f }, .05f, { .060f, .044f, .011f } },
  [MATERIAL_GRAVEL] = { { .60f, .70f, .80f }, .05f, { .031f, .012f, .008f } },
  [MATERIAL_METAL] = { { .20f, .07f, .06f }, .05f, { .200f, .025f, .010f } },
  [MATERIAL_PLASTER] = { { .12f, .06f, .04f }, .05f, { .056f, .056f, .004f } },
  [MATERIAL_ROCK] = { { .13f, .20f, .24f }, .05f, { .015f, .002f, .001f } },
  [MATERIAL_WOOD] = { { .11f, .07f, .06f }, .05f, { .070f, .014f, .005f } }
};

bool phonon_setGeometry(float* vertices, uint32_t* indices, uint32_t vertexCount, uint32_t indexCount, AudioMaterial material) {
  if (state.mesh) {
    phonon_iplStaticMeshRemove(state.mesh, state.scene);
    phonon_iplSceneCommit(state.scene);
    phonon_iplStaticMeshRelease(&state.mesh);
    state.mesh = NULL;
  }

  uint32_t triangleCount = indexCount / 3;
  IPLint32* materialIndices = malloc(triangleCount * sizeof(IPLint32));
  if (!materialIndices) return false;

  for (uint32_t i = 0; i < triangleCount; i++) {
    materialIndices[i] = material;
  }

  IPLStaticMeshSettings settings = {
    .numVertices = vertexCount,
    .numTriangles = triangleCount,
    .numMaterials = sizeof(materials) / sizeof(materials[0]),
    .vertices = (IPLVector3*) vertices,
    .indices = (IPLTriangle*) indices,
    .materialIndices = materialIndices,
    .materials = materials
  };

  if (phonon_iplStaticMeshCreate(state.scene, &settings, &state.mesh) != IPL_STATUS_SUCCESS) {
    free(materialIndices);
    state.mesh = NULL;
    return false;
  }

  phonon_iplStaticMeshAdd(state.mesh, state.scene);
  phonon_iplSceneCommit(state.scene);
  free(materialIndices);
  return true;
}

void phonon_sourceCreate(Source* source) {
  uint32_t index = lovrSourceGetIndex(source);

  if (!state.binauralEffect[index]) {
    phonon_iplCreateBinauralEffect(state.binauralRenderer, MONO, STEREO, &state.binauralEffect[index]);
  }

  if (!state.directSoundEffect[index]) {
    phonon_iplCreateDirectSoundEffect(MONO, MONO, state.renderingSettings, &state.directSoundEffect[index]);
  }

  if (!state.convolutionEffect[index]) {
    IPLBakedDataIdentifier id = { 0 };
    phonon_iplCreateConvolutionEffect(state.environmentalRenderer, id, IPL_SIMTYPE_REALTIME, MONO, AMBISONIC, &state.convolutionEffect[index]);
  }
}

void phonon_sourceDestroy(Source* source) {
  uint32_t index = lovrSourceGetIndex(source);
  if (state.binauralEffect[index]) phonon_iplFlushBinauralEffect(state.binauralEffect[index]);
  if (state.directSoundEffect[index]) phonon_iplFlushDirectSoundEffect(state.directSoundEffect[index]);
  if (state.convolutionEffect[index]) phonon_iplFlushConvolutionEffect(state.convolutionEffect[index]);
}

Spatializer phononSpatializer = {
  .init = phonon_init,
  .destroy = phonon_destroy,
  .apply = phonon_apply,
  .tail = phonon_tail,
  .setListenerPose = phonon_setListenerPose,
  .setGeometry = phonon_setGeometry,
  .sourceCreate = phonon_sourceCreate,
  .sourceDestroy = phonon_sourceDestroy,
  .name = "phonon"
};
