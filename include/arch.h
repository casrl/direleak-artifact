#ifndef ARCH_H
#define ARCH_H
#if   ARCH == 4
#  include "arch_spr.h"
#elif ARCH == 3
#  include "arch_icx.h"
#elif ARCH == 2
#  ifdef ARCH_SKX
#    include "arch_skx.h"
#  else
#    include "arch_clx.h"
#  endif
#else
#  error "Unknown ARCH — build with ARCH=spr|icx|clx|skx"
#endif
#endif /* ARCH_H */
