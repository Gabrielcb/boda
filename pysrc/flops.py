# pretty printing stuff. factor out somewhere?
def pp_val_part( v, force ):
    if v < 10: return "%.2f" % v
    if v < 100: return "%.1f" % v
    if (v < 1000) or force: return "%.0f" % v
    return None
def pp_val( v ): # pretty-print flops
    ret = pp_val_part( v, 0 )
    exp = 0
    while ret is None:
        v /= 1000.0
        exp += 1
        ret = pp_val_part( v, exp == 5 )
    return ret+" KMGTP"[exp]
def pp_flops( flops ): return pp_val( flops ) + " FLOPS" # pretty-print flops

# cnet flop-calculating ops classes
class NDA( object ): 
    def __init__( self, num, chan, y, x ): 
        self.num = num
        self.chan = chan
        self.y = y
        self.x = x
    def dims_prod( self ): return self.num*self.chan*self.y*self.x

tot_forward_flops = 0
class Convolution( object ): 
    def __init__( self, name, bots, tops, filts, biases, in_pad, stride ): 
        # note: ignores in_pad and stride, but they sort-of aren't
        # needed since the output size is calculated using them. we
        # could use them as a check here, but that would require
        # duplicating the in-out calculation code?
        assert len(bots) == 1
        assert len(tops) == 1
        bot = bots[0]
        top = tops[0]
        in_pels = bot.dims_prod()
        out_pels = top.dims_prod()
        assert bot.chan == filts.chan # filt.chan is (or should be) input chans
        assert top.chan == filts.num # filt.num is (or should be) output chans
        # note: top.{x,y} should be = ( bot.{x,y} + pad ) / stride   (ceild/floord correctly)
        forward_flops = out_pels * filts.x * filts.y * filts.chan
        grad_inner_dim = out_pels / filts.num # aka number of input patches
        assert grad_inner_dim == top.num*top.x*top.y
        back_grad_flops = filts.dims_prod() * grad_inner_dim # grad is same size as filts
        diff_inner_dim = filts.num
        # diff ends up as the same size as input but is reduced from a temp of size im2col(input).
        back_diff_flops = (filts.chan*filts.x*filts.y)*diff_inner_dim*grad_inner_dim  # as: (M) * N * K

        print name," FORWARD",pp_flops(forward_flops)," --- BACK_GRAD",pp_flops(back_grad_flops),
        print " --- BACK_DIFF",pp_flops(back_diff_flops)
        global tot_forward_flops
        tot_forward_flops += forward_flops

InnerProduct=Convolution

class Pooling( object ): 
    def __init__( self, **kwargs ): self.opts = kwargs
class LRN( object ): 
    def __init__( self, **kwargs ): self.opts = kwargs
class Concat( object ): 
    def __init__( self, **kwargs ): self.opts = kwargs

# set num_img and source cnet decl
import sys
num_img = int(sys.argv[1])
execfile( "out.py" )

print "\nTOTAL_FORWARD",pp_flops(tot_forward_flops)
