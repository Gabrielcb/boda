extern "C"  __global__ void %(cu_func_name)( float const * const in, float * const out ) {
  uint32_t const out_ix = blockDim.x * blockIdx.x + threadIdx.x;
  if( out_ix >= %(out_ix_sz) ) { return; }
  float out_v = 0.0f;
  uint32_t const in_ix = %(out_ix_img) * %(in_ix_img_sz) + %(out_ix_chan) * %(in_ix_chan_sz) + 
    %(out_ix_y)*%(in_ix_y_sz)*%(stride) + %(out_ix_x)*%(in_ix_x_sz)*%(stride);
  %(ops);
  out[out_ix] = out_v;
}
