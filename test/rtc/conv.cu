// 256 tbp
// each thread: computes 8x8 block of out
// loop over k dim
extern "C"  __global__ void %(cu_func_name)( float const * const filts, float const * const biases, float const * const in, float * const out ) {
  __shared__ float in_smem[%(threadIdx.x_patch_tile_dim)*%(t_tile_sz)];
  __shared__ float filts_smem[%(threadIdx.x_out_chan_tile_dim)*%(t_tile_sz)];
  float out_tile[%(t_tile_sz)*%(t_tile_sz)] = {0}; // tile of output for this thread to compute, stored in registers
  // reg. buffers for one strip each from in and filts of %(t_tile_sz) elements, for the same filts_ix_out_chan_elem
  float filts_strip[%(t_tile_sz)]; // across output chans (stride is %(filts_ix_out_chan_sz) )
  float in_strip[%(t_tile_sz)]; // across patches (approx square block in x/y space, favoring x if sqrt() not integer)
  uint32_t const blk_filt_ix_sz = %(threadIdx.x_out_chan_tile_dim)*%(t_tile_sz);
  uint32_t const blk_filt_ix_base = %(blockIdx.x_out_chan_blk)*blk_filt_ix_sz;

  uint32_t const blk_patch_ix_sz = %(threadIdx.x_patch_tile_dim)*%(t_tile_sz);
  uint32_t const blk_patch_ix_base = %(blockIdx.x_patch_blk)*blk_patch_ix_sz;

  // iteratate over filter elements
  for( uint32_t filts_ix_out_chan_elem = 0; filts_ix_out_chan_elem != %(filts_ix_out_chan_sz); ++filts_ix_out_chan_elem ) {
    // (1) load %(t_tile_sz) elements from in and filts    
    __syncthreads();
    if( threadIdx.x < blk_filt_ix_sz ) { 
      filts_smem[threadIdx.x] = filts[(blk_filt_ix_base+threadIdx.x)*%(filts_ix_out_chan_sz) + filts_ix_out_chan_elem];
    }
    if( threadIdx.x < blk_patch_ix_sz ) { 
      uint32_t const t_smem_patch_ix = (blk_patch_ix_base+threadIdx.x);
      in_smem[threadIdx.x] = in[%(t_smem_patch_ix_img)*%(in_ix_img_sz) +
				%(filts_ix_out_chan_elem_in_chan)*%(in_ix_chan_sz) +
				(%(t_smem_patch_ix_y)*%(stride)+%(filts_ix_out_chan_elem_y))*%(in_ix_y_sz) +
				(%(t_smem_patch_ix_x)*%(stride)+%(filts_ix_out_chan_elem_x))*%(in_ix_x_sz)];
    }
    if( 0 && threadIdx.x+blockDim.x < blk_patch_ix_sz ) { 
      uint32_t const t_smem_patch_ix = (blk_patch_ix_base+threadIdx.x+blockDim.x);
      in_smem[threadIdx.x+blockDim.x] = in[%(t_smem_patch_ix_img)*%(in_ix_img_sz) +
					   %(filts_ix_out_chan_elem_in_chan)*%(in_ix_chan_sz) +
					   (%(t_smem_patch_ix_y)*%(stride)+%(filts_ix_out_chan_elem_y))*%(in_ix_y_sz) +
					   (%(t_smem_patch_ix_x)*%(stride)+%(filts_ix_out_chan_elem_x))*%(in_ix_x_sz)];
    }
    __syncthreads();
    %(t_tile_loads);
    // (2) do %(t_tile_sz)^2 fmas into out_tile
    %(t_tile_fmas);
  }
  // add bias to each elem of out_tile[] and store the results to out[]
  %(t_tile_stores);
}

