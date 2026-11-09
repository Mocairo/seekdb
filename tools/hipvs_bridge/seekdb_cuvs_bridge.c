/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
} sk_cuvs_index;

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 64;
  h->ip->intermediate_graph_degree = 128;
  h->ip->nn_descent_niter = 10;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  float* qd = 0; unsigned* nd = 0; float* dd = 0; cuvsCagraSearchParams_t sp = 0;
  if (cuvsRMMAlloc(h->res, (void**)&qd, sizeof(float)*nq*h->dim)  != CUVS_SUCCESS) { rc=10; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&nd, sizeof(unsigned)*nq*topk) != CUVS_SUCCESS) { rc=11; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&dd, sizeof(float)*nq*topk)    != CUVS_SUCCESS) { rc=12; goto done; }
  if (cudaMemcpy(qd, query, sizeof(float)*nq*h->dim, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { rc=14; goto done; }
    {
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 2048) required_itopk = 2048;
      if (sp->itopk_size < required_itopk) sp->itopk_size = required_itopk;
      sp->search_width = 4;
      sp->algo = MULTI_CTA;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist) cudaMemcpy(out_dist, dd, sizeof(float)*nq*topk, cudaMemcpyDefault);
  }
done:
  if (sp) cuvsCagraSearchParamsDestroy(sp);
  if (dd) cuvsRMMFree(h->res, dd, sizeof(float)*nq*topk);
  if (nd) cuvsRMMFree(h->res, nd, sizeof(unsigned)*nq*topk);
  if (qd) cuvsRMMFree(h->res, qd, sizeof(float)*nq*h->dim);
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}

/* EXPERIMENT ONLY. Append to the exact f9 bridge in a new cloud source copy.
 * No new hipVS hard references: absent optional symbols disable this API.
 * The existing build/search/free/file API and all algorithm parameters stay.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>

typedef struct RawGraphSnapshotApiV1 {
  uint32_t abi_version;
  uint32_t struct_size;
  int (*shape)(void *, long *, long *, long *);
  int (*export_graph)(void *, uint32_t *, size_t);
  void *(*import_graph)(const float *, long, long, const uint32_t *, long);
} RawGraphSnapshotApiV1;

typedef cuvsError_t (*RawGetGraphFn)(cuvsCagraIndex_t, DLManagedTensor *);
typedef cuvsError_t (*RawFromArgsFn)(cuvsResources_t, cuvsDistanceType,
                                   DLManagedTensor *, DLManagedTensor *, cuvsCagraIndex_t);
typedef cuvsError_t (*RawStreamSyncFn)(cuvsResources_t);
typedef struct RawGraphFunctions {
  RawGetGraphFn graph;
  RawFromArgsFn from_args;
  RawStreamSyncFn sync;
} RawGraphFunctions;

static int raw_graph_functions(RawGraphFunctions *functions)
{
  void *address;
  memset(functions, 0, sizeof(*functions));
  /* Linux/POSIX dlsym representation is checked before memcpy into function pointers. */
  if (sizeof(address) != sizeof(functions->graph) || sizeof(address) != sizeof(functions->from_args)
      || sizeof(address) != sizeof(functions->sync)) return 1;
  (void)dlerror();
  address = dlsym(RTLD_DEFAULT, "cuvsCagraIndexGetGraph");
  if (dlerror() != NULL || address == NULL) return 1;
  memcpy(&functions->graph, &address, sizeof(address));
  (void)dlerror();
  address = dlsym(RTLD_DEFAULT, "cuvsCagraIndexFromArgs");
  if (dlerror() != NULL || address == NULL) return 1;
  memcpy(&functions->from_args, &address, sizeof(address));
  (void)dlerror();
  address = dlsym(RTLD_DEFAULT, "cuvsStreamSync");
  if (dlerror() != NULL || address == NULL) return 1;
  memcpy(&functions->sync, &address, sizeof(address));
  return 0;
}

static int raw_graph_extent(long n, long dim, long degree, size_t *count)
{
  if (n < 256 || (uint64_t)n > UINT32_MAX || dim <= 0 || degree <= 0 || degree >= n
      || (size_t)n > SIZE_MAX / (size_t)dim / sizeof(float)
      || (size_t)n > SIZE_MAX / (size_t)degree / sizeof(uint32_t)) return 1;
  *count = (size_t)n * (size_t)degree;
  return 0;
}

static void raw_graph_view_release(DLManagedTensor *tensor)
{
  if (tensor->deleter != NULL) tensor->deleter(tensor);
  memset(tensor, 0, sizeof(*tensor));
}

static int raw_graph_view(sk_cuvs_index *index, RawGraphFunctions *functions,
                          DLManagedTensor *tensor, long *degree)
{
  memset(tensor, 0, sizeof(*tensor));
  if (index == NULL || raw_graph_functions(functions) != 0) return 1;
  if (functions->graph(index->idx, tensor) != CUVS_SUCCESS) {
    raw_graph_view_release(tensor);
    return 1;
  }
  DLTensor *view = &tensor->dl_tensor;
  size_t count = 0;
  if (view->data == NULL || view->ndim != 2 || view->shape == NULL
      || view->dtype.code != kDLUInt || view->dtype.bits != 32 || view->dtype.lanes != 1
      || view->device.device_type != kDLCUDA || view->shape[0] != index->n
      || view->shape[1] <= 0 || (uint64_t)view->shape[1] > LONG_MAX
      || (view->strides != NULL && (view->strides[0] != view->shape[1] || view->strides[1] != 1))
      || view->byte_offset % sizeof(uint32_t) != 0
      || view->byte_offset > UINTPTR_MAX - (uintptr_t)view->data
      || raw_graph_extent(index->n, index->dim, (long)view->shape[1], &count) != 0) {
    raw_graph_view_release(tensor);
    return 1;
  }
  uintptr_t start = (uintptr_t)view->data + (uintptr_t)view->byte_offset;
  if (count * sizeof(uint32_t) > UINTPTR_MAX - start) {
    raw_graph_view_release(tensor);
    return 1;
  }
  *degree = (long)view->shape[1];
  return 0;
}

static int raw_graph_shape(void *handle, long *n, long *dim, long *degree)
{
  sk_cuvs_index *index = (sk_cuvs_index *)handle;
  RawGraphFunctions functions;
  DLManagedTensor tensor;
  long observed_degree = 0;
  if (n == NULL || dim == NULL || degree == NULL
      || raw_graph_view(index, &functions, &tensor, &observed_degree) != 0) return 1;
  *n = index->n;
  *dim = index->dim;
  *degree = observed_degree;
  raw_graph_view_release(&tensor);
  return 0;
}

static int raw_graph_export(void *handle, uint32_t *destination, size_t count)
{
  sk_cuvs_index *index = (sk_cuvs_index *)handle;
  RawGraphFunctions functions;
  DLManagedTensor tensor;
  long degree = 0;
  size_t expected_count = 0;
  int rc = 1;
  if (destination == NULL || raw_graph_view(index, &functions, &tensor, &degree) != 0) return 1;
  if (raw_graph_extent(index->n, index->dim, degree, &expected_count) == 0 && count == expected_count
      && functions.sync(index->res) == CUVS_SUCCESS) {
    const char *source = (const char *)tensor.dl_tensor.data + tensor.dl_tensor.byte_offset;
    if (cudaMemcpy(destination, source, count * sizeof(uint32_t), cudaMemcpyDeviceToHost) == cudaSuccess) {
      rc = 0;
      for (size_t i = 0; i < count; ++i) {
        if (destination[i] >= (uint32_t)index->n) { rc = 1; break; }
      }
    }
  }
  /* Only the descriptor is freed; the CAGRA index remains the graph owner. */
  raw_graph_view_release(&tensor);
  return rc;
}

static void raw_cpu_tensor(DLManagedTensor *tensor, void *data, int64_t *shape, uint8_t code)
{
  memset(tensor, 0, sizeof(*tensor));
  tensor->dl_tensor.data = data;
  tensor->dl_tensor.device.device_type = kDLCPU;
  tensor->dl_tensor.device.device_id = 0;
  tensor->dl_tensor.ndim = 2;
  tensor->dl_tensor.dtype.code = code;
  tensor->dl_tensor.dtype.bits = 32;
  tensor->dl_tensor.dtype.lanes = 1;
  tensor->dl_tensor.shape = shape;
}

static void *raw_graph_import(const float *base, long n, long dim, const uint32_t *graph, long degree)
{
  RawGraphFunctions functions;
  size_t count = 0;
  sk_cuvs_index *index = NULL;
  DLManagedTensor dataset_tensor, graph_tensor;
  int64_t dataset_shape[2] = {n, dim}, graph_shape[2] = {n, degree};
  int64_t actual_n = 0, actual_dim = 0;
  long actual_degree = 0;
  if (base == NULL || graph == NULL || raw_graph_extent(n, dim, degree, &count) != 0
      || raw_graph_functions(&functions) != 0) return NULL;
  for (size_t i = 0; i < count; ++i) if (graph[i] >= (uint32_t)n) return NULL;
  index = (sk_cuvs_index *)calloc(1, sizeof(*index));
  if (index == NULL) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  raw_cpu_tensor(&dataset_tensor, (void *)base, dataset_shape, kDLFloat);
  raw_cpu_tensor(&graph_tensor, (void *)graph, graph_shape, kDLUInt);
  /* Fixed _from_args selects host/host and synchronizes before returning.
   * Host graph is copied. Dataset ownership also depends on pointer attributes
   * and stride: caller retains base until this index is destroyed. No Build. */
  if (functions.from_args(index->res, L2Expanded, &graph_tensor, &dataset_tensor, index->idx) != CUVS_SUCCESS
      || functions.sync(index->res) != CUVS_SUCCESS
      || cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS
      || cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS
      || actual_n != n || actual_dim != dim) goto fail_idx;
  /* GetGraphDegree is optional too: inspect the returned graph descriptor. */
  index->n = n; index->dim = dim;
  {
    DLManagedTensor verified_graph;
    if (raw_graph_view(index, &functions, &verified_graph, &actual_degree) != 0) goto fail_idx;
    raw_graph_view_release(&verified_graph);
  }
  if (actual_degree != degree) goto fail_idx;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  free(index);
  return NULL;
}

#ifdef __cplusplus
extern "C" {
#endif
const RawGraphSnapshotApiV1 *seekdb_cuvs_raw_graph_snapshot_v1(void)
{
  static const RawGraphSnapshotApiV1 api = {1, sizeof(RawGraphSnapshotApiV1),
                                         raw_graph_shape, raw_graph_export, raw_graph_import};
  RawGraphFunctions functions;
  return raw_graph_functions(&functions) == 0 ? &api : NULL;
}
#ifdef __cplusplus
}
#endif
