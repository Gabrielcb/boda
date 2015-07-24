extern "C"  __global__ void %(cu_func_name)( float const * const in, float * const out ) {
  int32_t const in_ix = blockDim.x * blockIdx.x + threadIdx.x;
  if( in_ix >= %(in_sz) ) { return; }
  out[in_ix] = in[in_ix];
}

/*

in_pels = num_img * in.sz.dims_prod()
num_in_blks = u32_ceil_div( in_pels, block_chan_pels )

normal in dims: img, chan, y, x OR img, chan, pels // where pels = x,y dims merged

block_iters = u32_ceil_div( chan, in_chan_tile ) // for ccp1, 96/8=12
pad_chan =  block_iter * in_chan_tile // pad by up to (in_chan_tile-1) [typ. 8; pad with zeros? garbage okay?]
block_chan_pels = t_tile_sz*tix_pels_tile_sz // typically 8*8=64
block_iter_pels = block_chan_pels * in_chan_tile; // typically 512

block_pels = 12*512 = 6144 // note: 24576 bytes, prob. too big for SM to fully cache, but 512=2K (per-iter cache) is fine.


xposed in dims (inner): (block_iter,  block_iter_chan, block_iter_pel)  == block_pel
            sz (inner): (block_iters, in_chan_tile,    block_chan_pels) == block_pels (only inner 2 dims need to be linear?)

*/
