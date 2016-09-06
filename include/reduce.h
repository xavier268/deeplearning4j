#include <dll.h>
//#include <string>
#include <sharedmem.h>
#include <stdio.h>
#include <shape.h>
#include <omp.h>
#include <templatemath.h>
#include <helper_cuda.h>
#include <nd4jmalloc.h>
#include <pairwise_util.h>
#include <ops.h>
#include <op_boilerplate.h>

#pragma once
#ifdef __CUDACC__
#include <cuda.h>
#include <cuda_runtime.h>
#endif
#ifdef __JNI__
#include <jni.h>
#endif


#define REDUCE_OPS \
        (0, simdOps::Mean), \
        (1, simdOps::Sum), \
        (3, simdOps::Max), \
        (4, simdOps::Min), \
        (5, simdOps::Norm1), \
        (6, simdOps::Norm2), \
        (7, simdOps::NormMax), \
        (8, simdOps::Prod), \
        (9, simdOps::StandardDeviation), \
        (10,simdOps::Variance), \
		(11, simdOps::ASum), \
        (12, simdOps::MatchCondition)

        
//an op for the kernel
namespace functions {
    namespace reduce {

/**
 * A reduce function
 * reduces a vector down to
 * a subset of itself
 * via aggregating member
 * elements.
 */
        template<typename T>
		class ReduceFunction {
		public:
#ifdef __CUDACC__
template<typename OpType>
			static inline __device__ void transformCuda1D(T *dx,
				int *xShapeInfo,
				T *extraParams,
				T *result,
				int *resultShapeInfo,
				int *dimension,
				int dimensionLength,
				T *reductionBuffer, UnifiedSharedMemory *manager, int *tadOnlyShapeInfo, int *tadOffsets) {

				//shared memory space for storing intermediate results
				T *sPartials = (T *)manager->getSharedReductionBuffer();

				sPartials[threadIdx.x] = OpType::startingValue(dx);

				__shared__ int tadLength;
				__shared__ int tadEWS;
				__shared__ int numTads;
				if (threadIdx.x == 0) {
					tadLength = shape::tadLength(xShapeInfo, dimension, dimensionLength);
					tadEWS = shape::elementWiseStride(tadOnlyShapeInfo);
					numTads = shape::length(xShapeInfo) / tadLength;
				}
				__syncthreads();

				for (int r = blockIdx.x; r < numTads; r += gridDim.x) {
					int tadOffsetForBlock = tadOffsets[r];

					sPartials[threadIdx.x] = OpType::startingValue(dx + tadOffsetForBlock);

					for (int i = threadIdx.x; i < tadLength; i += blockDim.x) {
						sPartials[threadIdx.x] = OpType::update(sPartials[threadIdx.x], OpType::op(dx[tadOffsetForBlock + i * tadEWS], extraParams), extraParams);
					}
					__syncthreads();

					// aggregate. do NOT reduce for elements > tadLength
					aggregatePartials<OpType>(sPartials, threadIdx.x, nd4j::math::nd4j_min<int>(blockDim.x, tadLength), extraParams);


					__syncthreads();
					if (threadIdx.x == 0) {
						result[r] = OpType::postProcess(sPartials[threadIdx.x], tadLength, extraParams);
					}
				}
			}

template<typename OpType>
			static inline __device__ void execScalarCuda(
				T *dx,
				int *xShapeInfo,
				T *extraParams,
				T *result,
				int *resultShapeInfo,
				T *reductionBuffer,
				UnifiedSharedMemory *manager,
				int *tadOnlyShapeInfo) {
				int elementWiseStride = shape::elementWiseStride(xShapeInfo);

				int n = shape::length(xShapeInfo);

				int tid = blockDim.x * blockIdx.x + threadIdx.x;

				//shared memory space for storing intermediate results
				T *sPartials = (T *)manager->getSharedReductionBuffer();

				sPartials[threadIdx.x] = OpType::startingValue(dx);

				if (elementWiseStride >= 1) {
					for (Nd4jIndex i = tid; i < n; i += (blockDim.x * gridDim.x)) {
						sPartials[threadIdx.x] = OpType::update(sPartials[threadIdx.x], OpType::op(dx[i * elementWiseStride], extraParams), extraParams);
					}
				}
				else {
					int rank = shape::rank(xShapeInfo);
					int ind2sub[MAX_RANK];

					for (Nd4jIndex i = tid; i < n; i += blockDim.x * gridDim.x) {
						shape::ind2sub(rank, shape::shapeOf(xShapeInfo), i, ind2sub);
						int offset = shape::getOffset(0, shape::shapeOf(xShapeInfo), shape::stride(xShapeInfo), ind2sub, rank);
						sPartials[threadIdx.x] = OpType::update(sPartials[threadIdx.x], OpType::op(dx[offset], extraParams), extraParams);
						__syncthreads();
					}
				}

				__syncthreads();
				aggregatePartials<OpType>(sPartials, threadIdx.x, nd4j::math::nd4j_min<int>(blockDim.x, n), extraParams);


				__syncthreads();

				if (gridDim.x > 1) {
					unsigned int *tc = (unsigned int *)reductionBuffer;
					__shared__ bool amLast;
					int rank = shape::rank(xShapeInfo);
					tid = threadIdx.x;
					if (threadIdx.x == 0) {
						reductionBuffer[blockIdx.x] = sPartials[0];//this->postProcess(sPartials[0],n,extraParams);
					}
					__threadfence();
					__syncthreads();

					if (threadIdx.x == 0) {
						unsigned int ticket = atomicInc(&tc[4096], gridDim.x);
						amLast = (ticket == gridDim.x - 1);
					}

					__syncthreads();

					if (amLast) {
						tc[4096] = 0;

						sPartials[threadIdx.x] = OpType::startingValue(dx);

						for (Nd4jIndex i = threadIdx.x; i < gridDim.x; i += blockDim.x) {
							sPartials[threadIdx.x] = OpType::update(sPartials[threadIdx.x], reductionBuffer[i], extraParams);
						}
						__syncthreads();

						aggregatePartials<OpType>(sPartials, threadIdx.x, nd4j::math::nd4j_min<int>(gridDim.x, blockDim.x), extraParams);

						__syncthreads();
						if (threadIdx.x == 0) {
							result[0] = OpType::postProcess(sPartials[0], n, extraParams);
						}
					}
				}
				else {
					if (threadIdx.x == 0) {
						unsigned int *tc = (unsigned *)reductionBuffer;
						tc[4096] = 0;
						result[0] = OpType::postProcess(sPartials[0], n, extraParams);
					}
				}
			}
			/**
			 * Kernel invocation for reduce
			 * @param n the length of the buffer
			 * @param dx the input
			 * @param xShapeInfo the shape information for the input
			 * @param extraParams extra parameters (starting value,..)
			 * @param result the result buffer
			 * @param resultShapeInfo the shapeinformation for the result buffer
			 * @param gpuInformation the gpu information (shared memory allocated,..)
			 * @param dimension the dimension to do reduce along long
			 * @param dimensionLength the length of the dimension buffer
			 * @param postProcessOrNot whether to reduce or not
			 */
template<typename OpType>
			static inline __device__ void transformCuda6D(
				T *dx,
				int *xShapeInfo,
				T *extraParams,
				T *result,
				int *resultShapeInfo,
				int *dimension,
				int dimensionLength,
				T *reductionBuffer, UnifiedSharedMemory *manager, int *tadOnlyShapeInfo, int *tadOffsets) {

				//shared memory space for storing intermediate results
				T *sPartials = (T *)manager->getSharedReductionBuffer();

				__shared__ int tadLength;
				__shared__ int tadRank;
				__shared__ int numTads;
				__shared__ int *tadShape;
				__shared__ int *tadStride;
				if (threadIdx.x == 0) {
					tadLength = shape::tadLength(xShapeInfo, dimension, dimensionLength);
					tadRank = shape::rank(tadOnlyShapeInfo);
					numTads = shape::length(xShapeInfo) / tadLength;

					tadShape = shape::shapeOf(tadOnlyShapeInfo);
					tadStride = shape::stride(tadOnlyShapeInfo);
				}
				__syncthreads();

				int xCoord[3];

				for (int r = blockIdx.x; r < numTads; r += gridDim.x) {
					int tadOffsetForBlock = tadOffsets[r];

					sPartials[threadIdx.x] = OpType::startingValue(dx + tadOffsetForBlock);

					for (int i = threadIdx.x; i < tadLength; i += blockDim.x) {
						shape::ind2subC(tadRank, tadShape, i, xCoord);
						int xOffset = shape::getOffset(tadOffsetForBlock, tadShape, tadStride, xCoord, tadRank);

						sPartials[threadIdx.x] = OpType::update(sPartials[threadIdx.x], OpType::op(dx[xOffset], extraParams), extraParams);
					}
					__syncthreads();

					// aggregate. do NOT reduce for elements > tadLength
					aggregatePartials<OpType>(sPartials, threadIdx.x, nd4j::math::nd4j_min<int>(blockDim.x, tadLength), extraParams);

					__syncthreads();
					if (threadIdx.x == 0)
						result[r] = OpType::postProcess(sPartials[threadIdx.x], tadLength, extraParams);
				}
			}

template<typename OpType>
			static inline __device__ void transformCudaXD(
				T *dx,
				int *xShapeInfo,
				T *extraParams,
				T *result,
				int *resultShapeInfo,
				int *dimension,
				int dimensionLength,
				T *reductionBuffer,
				UnifiedSharedMemory *manager,
				int *tadOnlyShapeInfo,
				int *tadOffsets) {

				//shared memory space for storing intermediate results
				T *sPartials = (T *)manager->getSharedReductionBuffer();

				//                __shared__ shape::TAD *tad;
				__shared__ int tadLength;
				__shared__ int tadRank;
				__shared__ int numTads;
				__shared__ int *tadShape;
				__shared__ int *tadStride;
				if (threadIdx.x == 0) {
					tadLength = shape::tadLength(xShapeInfo, dimension, dimensionLength);
					tadRank = shape::rank(tadOnlyShapeInfo);
					numTads = shape::length(xShapeInfo) / tadLength;

					tadShape = shape::shapeOf(tadOnlyShapeInfo);
					tadStride = shape::stride(tadOnlyShapeInfo);
				}
				__syncthreads();

				int xCoord[MAX_RANK];

				for (int r = blockIdx.x; r < numTads; r += gridDim.x) {
					int tadOffsetForBlock = tadOffsets[r];

					sPartials[threadIdx.x] = OpType::startingValue(dx + tadOffsetForBlock);

					for (int i = threadIdx.x; i < tadLength; i += blockDim.x) {
						shape::ind2subC(tadRank, tadShape, i, xCoord);
						int xOffset = shape::getOffset(tadOffsetForBlock, tadShape, tadStride, xCoord, tadRank);

						sPartials[threadIdx.x] = OpType::update(sPartials[threadIdx.x], OpType::op(dx[xOffset], extraParams), extraParams);
					}
					__syncthreads();

					// aggregate. do NOT reduce for elements > tadLength
					aggregatePartials<OpType>(sPartials, threadIdx.x, nd4j::math::nd4j_min<int>(blockDim.x, tadLength), extraParams);


					__syncthreads();
					if (threadIdx.x == 0)
						result[r] = OpType::postProcess(sPartials[threadIdx.x], tadLength, extraParams);
				}
			}

			/**
			 *
			 * @param sPartialsRef
			 * @param tid
			 * @param extraParams
			 */
template<typename OpType>
			__device__ static inline void aggregatePartials(T *sPartials, int tid, int numItems, T *extraParams) {
				// start the shared memory loop on the next power of 2 less
				// than the block size.  If block size is not a power of 2,
				// accumulate the intermediate sums in the remainder range.
				int floorPow2 = numItems;

				if (floorPow2 & (floorPow2 - 1)) {
					while (floorPow2 & (floorPow2 - 1)) {
						floorPow2 &= floorPow2 - 1;
					}
					if (tid >= floorPow2) {
						sPartials[tid - floorPow2] = OpType::update(sPartials[tid - floorPow2], sPartials[tid], extraParams);
					}
					__syncthreads();
				}
				__syncthreads();

#pragma unroll
				for (int activeThreads = floorPow2 >> 1; activeThreads; activeThreads >>= 1) {
					if (tid < activeThreads && tid + activeThreads < numItems) {
						sPartials[tid] = OpType::update(sPartials[tid], sPartials[tid + activeThreads], extraParams);
					}
					__syncthreads();
				}

			}

			static inline __device__  void execScalarCuda(
			const int opNum,
			T *x,
			int *xShapeInfo,
			T *extraParams,
			T *result,
			int *resultShapeInfo,
			T *reductionBuffer,
			UnifiedSharedMemory *manager,
			int *tadShapeInfo) {
                            DISPATCH_BY_OPNUM(execScalarCuda, PARAMS(x, xShapeInfo, extraParams, result, resultShapeInfo, reductionBuffer, manager, tadShapeInfo), REDUCE_OPS);
			}



			static inline __device__ void transformCudaXD(
				const int opNum,
				T *x,
				int *xShapeInfo,
				T *extraParams,
				T *result,
				int *resultShapeInfo,
				int *dimension,
				int dimensionLength,
				T *reductionBuffer,
				UnifiedSharedMemory *manager,
				int *tadShapeInfo,
				int *tadOffset) {
                            DISPATCH_BY_OPNUM(transformCudaXD, PARAMS(x, xShapeInfo, extraParams, result, resultShapeInfo, dimension, dimensionLength, reductionBuffer, manager, tadShapeInfo, tadOffset), REDUCE_OPS);
			}


			static inline __device__ void transformCuda6D(
				const int opNum,
				T *x,
				int *xShapeInfo,
				T *extraParams,
				T *result,
				int *resultShapeInfo,
				int *dimension,
				int dimensionLength,
				T *reductionBuffer,
				UnifiedSharedMemory *manager,
				int *tadShapeInfo,
				int *tadOffset) {
                            DISPATCH_BY_OPNUM(transformCuda6D, PARAMS(x, xShapeInfo, extraParams, result, resultShapeInfo, dimension, dimensionLength, reductionBuffer, manager, tadShapeInfo, tadOffset), REDUCE_OPS);
			}

			static inline __device__ void transformCuda1D(
				const int opNum,
				T *x,
				int *xShapeInfo,
				T *extraParams,
				T *result,
				int *resultShapeInfo,
				int *dimension,
				int dimensionLength,
				T *reductionBuffer,
				UnifiedSharedMemory *manager,
				int *tadShapeInfo,
				int *tadOffset) {
                            DISPATCH_BY_OPNUM(transformCuda1D, PARAMS(x, xShapeInfo, extraParams, result, resultShapeInfo, dimension, dimensionLength, reductionBuffer, manager, tadShapeInfo, tadOffset), REDUCE_OPS);
			}
#endif

			static T execScalar(const int opNum, T *x, int *xShapeInfo, T *extraParams) {
                            RETURNING_DISPATCH_BY_OPNUM(execScalar, PARAMS(x, xShapeInfo, extraParams), REDUCE_OPS);
			}

			static void exec(const int opNum,
				T *x,
				int *xShapeInfo,
				T *extraParams,
				T *result,
				int *resultShapeInfoBuffer,
				int *dimension,
				int dimensionLength, int *tadShapeInfo, int *tadOffset) {
                            DISPATCH_BY_OPNUM(exec, PARAMS(x, xShapeInfo, extraParams, result, resultShapeInfoBuffer, dimension, dimensionLength, tadShapeInfo, tadOffset), REDUCE_OPS);
			}

			/**
			 * Reduce down to 1 number
			 * @param x the input
			 * @param xShapeInfo the shape information
			 * for the input
			 * @param extraParams the extra params
			 * @return
			 */
			template<typename OpType>
#ifdef __CUDACC__
			__host__
#endif
			static T execScalar(T *x, int *xShapeInfo, T *extraParams) {
				const Nd4jIndex length = shape::length(xShapeInfo);
				int xElementWiseStride = shape::elementWiseStride(xShapeInfo);
				if (xElementWiseStride >= 1) {
					return execScalar<OpType>(x, xElementWiseStride, length, extraParams);
				}
				else {
					int shapeIter[MAX_RANK];
					int coord[MAX_RANK];
					int dim;
					int xStridesIter[MAX_RANK];

					int *xShape = shape::shapeOf(xShapeInfo);
					int *xStride = shape::stride(xShapeInfo);
					T start = OpType::startingValue(x);
					int rank = shape::rank(xShapeInfo);

					if (PrepareOneRawArrayIter<T>(rank,
						xShape,
						x,
						xStride,
						&rank,
						shapeIter,
						&x,
						xStridesIter) >= 0) {

						ND4J_RAW_ITER_START(dim, rank, coord, shapeIter); {
							/* Process the innermost dimension */
							const T *xIter = x;
							start = OpType::update(start, OpType::op(xIter[0], extraParams), extraParams);
						}
						ND4J_RAW_ITER_ONE_NEXT(dim,
							rank,
							coord,
							shapeIter,
							x,
							xStridesIter);
						start = OpType::postProcess(start, shape::length(xShapeInfo), extraParams);
					}
					else {
						printf("Unable to prepare array\n");
					}

					return start;

				}

			}

			/**
			 * Execute on the cpu
			 * @param x the input data
			 * @param xShapeInfo the shape information for x
			 * @param extraParams the extra parameters
			 * @param result the result buffer
			 * @param resultShapeInfoBuffer the shape information
			 * @param dimension the dimension to perform
			 * the reduce along long
			 * @param dimensionLength the length of the dimension buffer
			 */


template<typename OpType>
#ifdef __CUDACC__
			__host__
#endif
			static void exec(T *x,
				int *xShapeInfo,
				T *extraParams,
				T *result,
				int *resultShapeInfoBuffer,
				int *dimension,
				int dimensionLength, int *tadShapeInfo, int *tadOffset) {

				int resultLength = shape::length(resultShapeInfoBuffer);

				//pre squeezed: this is for keeping the pointer to the original
				//shape information for tad offset
				//the squeezed information doesn't render the right strides for
				//tad offset
				// || tad.wholeThing
				if (resultLength == 1 || dimension == nullptr || dimensionLength == shape::rank(xShapeInfo)) {
					result[0] = execScalar<OpType>(x, xShapeInfo, extraParams);
					return;
				}

				int *tadOnlyShapeInfo = tadShapeInfo;
				int *tadOffsets = tadOffset;
				shape::TAD *tad = nullptr;

				if (tadOnlyShapeInfo == nullptr || tadOffsets == nullptr) {
					tad = new shape::TAD(xShapeInfo, dimension, dimensionLength);
					tad->createTadOnlyShapeInfo();
					tad->createOffsets();

					if (tad->dimensionLength < 1) {
						delete tad;
						return;
					}

					tadOnlyShapeInfo = tad->tadOnlyShapeInfo;
					tadOffsets = tad->tadOffsets;
				}


				int tadRank = shape::rank(tadOnlyShapeInfo);
				const int tadLength = shape::tadLength(xShapeInfo, dimension, dimensionLength);
				int numTads = shape::length(xShapeInfo) / tadLength;
				int tadEWS = shape::elementWiseStride(tadOnlyShapeInfo);

				int tadsPerThread = resultLength / TAD_THRESHOLD;
				int num_threads = nd4j::math::nd4j_max<int>(1, tadsPerThread);
				num_threads = nd4j::math::nd4j_min<int>(num_threads, omp_get_max_threads());

				if (tadEWS > 0 && (numTads == 1 || shape::isVector(tadOnlyShapeInfo) || shape::isScalar(tadOnlyShapeInfo))) {

#pragma omp parallel for schedule(guided) num_threads(num_threads) if (num_threads > 1)
					for (int i = 0; i < resultLength; i++) {
						T *iter = x + tadOffsets[i];
						T start = OpType::startingValue(iter);
						if (tadEWS == 1) {

#pragma omp simd
							for (int j = 0; j < tadLength; j++) {
								start = OpType::update(start, OpType::op(iter[j], extraParams), extraParams);

							}
						}
						else {
#pragma omp simd
							for (int j = 0; j < tadLength; j++) {
								start = OpType::update(start, OpType::op(iter[j * tadEWS], extraParams), extraParams);
							}
						}
						result[i] = OpType::postProcess(start, tadLength, extraParams);
					}
				}
				else {
					int *tadShape = shape::shapeOf(tadOnlyShapeInfo);
					int *tadStride = shape::stride(tadOnlyShapeInfo);

#pragma omp  parallel for schedule(guided) num_threads(num_threads) if (num_threads > 1)
					for (int i = 0; i < resultLength; i++) {
						int offset = tadOffsets[i];
						int xCoord[MAX_RANK];

						T start = OpType::startingValue(x + offset);

						for (int j = 0; j < tadLength; j++) {
							shape::ind2subC(tadRank, tadShape, j, xCoord);
							int xOffset = shape::getOffset(offset, tadShape, tadStride, xCoord, tadRank);

							start = OpType::update(start, OpType::op(x[xOffset], extraParams), extraParams);
						}

						result[i] = OpType::postProcess(start, tadLength, extraParams);;
					}
				}

				if (tad != nullptr)
					delete tad;
			}





			/**
			* CPU implementation
			* @param x the input data
			* @param xShapeInfo the shape information for
			* the input data
			* @param extraParams the extra parameters for the problem
			* @param result the result buffer
			* @param resultShapeInfo the shape information
			*/
template<typename OpType>
#ifdef __CUDACC__
			__host__
#endif
			static void exec(T *x,
				int *xShapeInfo,
				T *extraParams,
				T *result,
				int *resultShapeInfo) {
				return execScalar<OpType>(x, xShapeInfo, extraParams);
			}



			/**
			* Reduce down to 1 number
			* @param x the input
			* @param xShapeInfo the shape information
			* for the input
			* @param extraParams the extra params
			* @return
			*/
			template<typename OpType>
#ifdef __CUDACC__
			__host__
#endif
			static T execScalar(const T *x, int xElementWiseStride, Nd4jIndex length, T *extraParams) {
				T startingVal = OpType::startingValue(x);
				if (xElementWiseStride == 1) {
					if (length < ELEMENT_THRESHOLD) {
						T local = OpType::startingValue(x);
#pragma omp simd
						for (Nd4jIndex i = 0; i < length; i++) {
							T curr = OpType::op(x[i], extraParams);
							local = OpType::update(local, curr, extraParams);

						}
						local = OpType::postProcess(local, length, extraParams);

						return local;
					}

					else {
						T finalVal = startingVal;
						BlockInformation info(length);
						T *blocks = new T[info.chunks];
#pragma omp parallel
						{
							T local = OpType::startingValue(x);
							for (int i = omp_get_thread_num(); i < info.chunks; i += info.threads) {
								Nd4jIndex newOffset = (i * info.items);
								const T *chunk = x + newOffset;
								Nd4jIndex itemsToLoop = info.items;
								if (newOffset >= length) {
									break;
								}

								//handle modulo case
								if (newOffset + info.items >= length) {
									itemsToLoop = length - newOffset;
								}
#pragma omp simd
								for (Nd4jIndex j = 0; j < itemsToLoop; j++) {
									T curr = OpType::op(chunk[j], extraParams);
									local = OpType::update(local, curr, extraParams);
								}

							}

							blocks[omp_get_thread_num()] = local;
						}

#pragma omp simd
						for (int i = 0; i < info.threads; i++) {
							finalVal = OpType::update(finalVal, blocks[i], extraParams);
						}


						finalVal = OpType::postProcess(finalVal, length, extraParams);
						delete[] blocks;
						return finalVal;

					}

				}

				else {
					if (length < 8000) {
						T local = OpType::startingValue(x);
#pragma omp simd
						for (Nd4jIndex i = 0; i < length; i++) {
							T curr = OpType::op(x[i * xElementWiseStride], extraParams);
							local = OpType::update(local, curr, extraParams);

						}

						local = OpType::postProcess(local, length, extraParams);

						return local;
					}

					T finalVal = startingVal;
					BlockInformation info(length);
					T *blocks = new T[info.chunks];


#pragma omp parallel
					{
						T local = OpType::startingValue(x);
						for (int i = omp_get_thread_num(); i < info.chunks; i += info.threads) {
							Nd4jIndex newOffset = (i * info.items) * xElementWiseStride;
							const T *chunk = x + newOffset;
							Nd4jIndex itemsToLoop = info.items;
#pragma omp simd

							for (Nd4jIndex i = 0; i < itemsToLoop; i++) {
								T curr = OpType::op(chunk[i * xElementWiseStride], extraParams);
								local = OpType::update(local, curr, extraParams);
							}


						}

						blocks[omp_get_thread_num()] = local;


					}

#pragma omp simd
					for (int i = 0; i < info.threads; i++) {
						finalVal = OpType::update(finalVal, blocks[i], extraParams);
					}

					finalVal = OpType::postProcess(finalVal, length, extraParams);
					delete[] blocks;
					return finalVal;

				}

			}
		};

#ifdef __CUDACC__
        /**
    *
    * @param extraParams
    * @param sPartials
    * @param sMemSize
    */
        template<typename T>
        __device__ void initializeShared(T *extraParams, T **sPartials, int sMemSize) {
            int sPartialsLength = sMemSize / sizeof(T);
            T *sPartialsDeref = (T *) *sPartials;
            for (int i = 0; i < sPartialsLength; i++) {
                sPartialsDeref[i] = extraParams[0];
            }
        }

#endif

    }

}


#ifdef __CUDACC__

/**
 * Interface for the c and driver api
 * @param op the operation number
 * @param n the length of the problem
 * @param dx  the input information
 * @param xShapeInfo the shape information
 * @param extraParams the extra parameters
 * @param result the result data
 * @param resultShapeInfo the result shape information
 * @param gpuInformation the gpu information
 * @param dimension the dimension to do reduce along long
 * @param dimensionLength the length of the dimension buffer
 * @param postProcessOrNot whether to pre process or not
 */
template <typename T>
__device__ void reduceGeneric(
        const int op,
        T *dx,
        int *xShapeInfo,
        T *extraParams,
        T *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        T *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {

    __shared__ UnifiedSharedMemory *manager;

    if (threadIdx.x == 0) {
        extern __shared__ unsigned char shmem[];
        manager = new(shmem) UnifiedSharedMemory((int *) shmem);
        manager->init(sizeof(UnifiedSharedMemory), 0, sizeof(functions::reduce::ReduceFunction<T>), sizeof(shape::TAD), shape::rank(xShapeInfo));
    }
    __syncthreads();


    functions::reduce::ReduceFunction<T>::transformCudaXD(
    		op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer,
            manager,
            tadOnlyShapeInfo,
            tadOffsets);
}

template <typename T>
__device__ void reduceGeneric1D(
        const int op,
        T *dx,
        int *xShapeInfo,
        T *extraParams,
        T *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        T *reductionBuffer,
        int *tadOnlyShapeInfo,
        int *tadOffsets) {

    __shared__ UnifiedSharedMemory *manager;

    if (threadIdx.x == 0) {
        extern __shared__ unsigned char shmem[];
        manager = new(shmem) UnifiedSharedMemory((int *) shmem);
        manager->init(sizeof(UnifiedSharedMemory), 0, sizeof(functions::reduce::ReduceFunction<T>), sizeof(shape::TAD), shape::rank(xShapeInfo));
    }

    __syncthreads();
    functions::reduce::ReduceFunction<T>::transformCuda1D(
    		op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, manager, tadOnlyShapeInfo, tadOffsets);
}

template <typename T>
__device__ void reduceGeneric6D(
        const int op,
        T *dx,
        int *xShapeInfo,
        T *extraParams,
        T *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        T *reductionBuffer,
        int *tadOnlyShapeInfo,
        int *tadOffsets) {

    extern __shared__ unsigned char shmem[];
    __shared__ UnifiedSharedMemory *manager;

    if (threadIdx.x == 0) {
        manager = new(shmem) UnifiedSharedMemory((int *) shmem);
        manager->init(sizeof(UnifiedSharedMemory), 0, sizeof(functions::reduce::ReduceFunction<T>), 16, shape::rank(xShapeInfo));
    }
    __syncthreads();


    functions::reduce::ReduceFunction<T>::transformCuda6D(
    		op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer,
            manager,
            tadOnlyShapeInfo,
            tadOffsets);
}

/**
 * Interface for the c and driver api
 * @param op the operation number
 * @param n the length of the problem
 * @param dx  the input information
 * @param xShapeInfo the shape information
 * @param extraParams the extra parameters
 * @param result the result data
 * @param resultShapeInfo the result shape information
 * @param gpuInformation the gpu information
 * @param dimension the dimension to do reduce along long
 * @param dimensionLength the length of the dimension buffer
 * @param postProcessOrNot whether to pre process or not
 */
extern "C" __global__ void reduceDouble(
        int op,
        double *dx,
        int *xShapeInfo,
        double *extraParams,
        double *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        double *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric<double>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer,
            tadOnlyShapeInfo,
            tadOffsets);

}

extern "C" __global__ void reduceDouble1D(
        int op,
        double *dx,
        int *xShapeInfo,
        double *extraParams,
        double *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        double *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric1D<double>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer,
            tadOnlyShapeInfo,
            tadOffsets);

}

extern "C" __global__ void reduceDouble6D(
        int op,
        double *dx,
        int *xShapeInfo,
        double *extraParams,
        double *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        double *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric6D<double>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer,
            tadOnlyShapeInfo,
            tadOffsets);

}

/**
 * Interface for the c and driver api
 * @param op the operation number
 * @param n the length of the problem
 * @param dx  the input information
 * @param xShapeInfo the shape information
 * @param extraParams the extra parameters
 * @param result the result data
 * @param resultShapeInfo the result shape information
 * @param gpuInformation the gpu information
 * @param dimension the dimension to do reduce along long
 * @param dimensionLength the length of the dimension buffer
 * @param postProcessOrNot whether to pre process or not
 */
extern "C" __global__ void reduceFloat(
        int op,
        float *dx,
        int *xShapeInfo,
        float *extraParams,
        float *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        float *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric<float>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer,
            tadOnlyShapeInfo,
            tadOffsets);
}

extern "C" __global__ void reduceHalf(
        int op,
        float16 *dx,
        int *xShapeInfo,
        float16 *extraParams,
        float16 *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        float16 *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric<float16>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer,
            tadOnlyShapeInfo,
            tadOffsets);
}

extern "C" __global__ void reduceFloat1D(
        int op,
        float *dx,
        int *xShapeInfo,
        float *extraParams,
        float *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        float *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric1D<float>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer,
            tadOnlyShapeInfo,
            tadOffsets);
}

extern "C" __global__ void reduceHalf1D(
        int op,
        float16 *dx,
        int *xShapeInfo,
        float16 *extraParams,
        float16 *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        float16 *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric1D<float16>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer,
            tadOnlyShapeInfo,
            tadOffsets);
}


extern "C" __global__ void reduceFloat6D(
        int op,
        float *dx,
        int *xShapeInfo,
        float *extraParams,
        float *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        float *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric6D<float>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer,
            tadOnlyShapeInfo,
            tadOffsets);
}

extern "C" __global__ void reduceHalf6D(
        int op,
        float16 *dx,
        int *xShapeInfo,
        float16 *extraParams,
        float16 *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        float16 *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric6D<float16>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer,
            tadOnlyShapeInfo,
            tadOffsets);
}

template <typename T>
__device__ void reduceScalarGeneric(
        int op,
        T *dx,
        int *xShapeInfo,
        T *extraParams,
        T *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        T *reductionBuffer, int *tadOnlyShapeInfo) {

    __shared__ UnifiedSharedMemory *manager;

    if (threadIdx.x == 0) {
        extern __shared__ unsigned char shmem[];
        manager = new(shmem) UnifiedSharedMemory((int *) shmem);
        manager->init(sizeof(UnifiedSharedMemory), 0, sizeof(functions::reduce::ReduceFunction<T>), sizeof(shape::TAD), 0);
    }
    __syncthreads();

    functions::reduce::ReduceFunction<T>::execScalarCuda(
    		op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            reductionBuffer,
            manager,
            tadOnlyShapeInfo);
};

extern "C" __global__ void reduceScalarFloat(
        int op,
        float *dx,
        int *xShapeInfo,
        float *extraParams,
        float *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        float *reductionBuffer, int *tadOnlyShapeInfo) {
    reduceScalarGeneric<float>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, tadOnlyShapeInfo);
};

extern "C" __global__ void reduceScalarHalf(
        int op,
        float16 *dx,
        int *xShapeInfo,
        float16 *extraParams,
        float16 *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        float16 *reductionBuffer, int *tadOnlyShapeInfo) {
    reduceScalarGeneric<float16>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, tadOnlyShapeInfo);
};

extern "C" __global__ void reduceScalarDouble(
        int op,
        double *dx,
        int *xShapeInfo,
        double *extraParams,
        double *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        double *reductionBuffer, int *tadOnlyShapeInfo) {
    reduceScalarGeneric<double>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, tadOnlyShapeInfo);
};
#endif

