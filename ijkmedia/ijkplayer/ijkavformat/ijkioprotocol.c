/*
 * Copyright (c) 2016 Bilibili
 * Copyright (c) 2016 Raymond Zheng <raymondzheng1412@gmail.com>
 *
 * This file is part of ijkPlayer.
 *
 * ijkPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ijkPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ijkPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
 #include "ijkioprotocol.h"
 #include <stdlib.h>
 #include <string.h>
 #include "libavutil/mem.h"
 #include "libavutil/log.h"
 
 extern IjkURLProtocol ijkio_ffio_protocol;
 extern IjkURLProtocol ijkio_cache_protocol;
 extern IjkURLProtocol ijkio_httphook_protocol;
#ifdef __ANDROID__
extern IjkURLProtocol ijkio_androidio_protocol;
#endif
 
 int ijkio_alloc_url(IjkURLContext **ph, const char *url) {
     if (!ph) {
         return -1;
     }
 
     IjkURLContext *h = av_mallocz(sizeof(IjkURLContext));
     if (!h)
         return AVERROR(ENOMEM);

     if (!strncmp(url, "cache:", strlen("cache:"))) {
         av_log(NULL, AV_LOG_INFO, "ijkioprotocol: use cache protocol for %s\n", url);
         h->prot = &ijkio_cache_protocol;
         h->priv_data = av_mallocz(ijkio_cache_protocol.priv_data_size);
     } else if (!strncmp(url, "ffio:", strlen("ffio:"))) {
         av_log(NULL, AV_LOG_INFO, "ijkioprotocol: use ffio protocol for %s\n", url);
         h->prot = &ijkio_ffio_protocol;
         h->priv_data = av_mallocz(ijkio_ffio_protocol.priv_data_size);
     } else if (!strncmp(url, "httphook:", strlen("httphook:")) || !strncmp(url, "http:", strlen("http:")) || !strncmp(url, "https:", strlen("https:"))) {
         av_log(NULL, AV_LOG_INFO, "ijkioprotocol: use httphook protocol for %s\n", url);
         h->prot = &ijkio_httphook_protocol;
         h->priv_data = av_mallocz(ijkio_httphook_protocol.priv_data_size);
#ifdef __ANDROID__
    } else if (!strncmp(url, "androidio:", strlen("androidio:"))) {
        av_log(NULL, AV_LOG_INFO, "ijkioprotocol: use androidio protocol for %s\n", url);
        h->prot = &ijkio_androidio_protocol;
        h->priv_data = av_mallocz(ijkio_androidio_protocol.priv_data_size);
#endif
     } else {
         av_log(NULL, AV_LOG_ERROR, "ijkioprotocol: unknown scheme for url %s\n", url);
         av_free(h);
         return -1;
     }
 
     av_log(NULL, AV_LOG_INFO, "ijkioprotocol: alloc_url success, protocol=%s, priv=%p\n", h->prot ? h->prot->name : "NULL", h->priv_data);
     *ph = h;
 
     return 0;
 }
 