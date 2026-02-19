package broker

import (
	"context"
	"crypto/tls"
	"net/http"
	"strings"
	"time"
)

func Serve(addr, tlsCert, tlsKey string, tlsCfg *tls.Config, h http.Handler) error {
	srv := &http.Server{
		Addr:              addr,
		Handler:           h,
		ReadHeaderTimeout: 5 * time.Second,
		TLSConfig:         tlsCfg,
	}
	if strings.TrimSpace(tlsCert) != "" && strings.TrimSpace(tlsKey) != "" {
		return srv.ListenAndServeTLS(strings.TrimSpace(tlsCert), strings.TrimSpace(tlsKey))
	}
	return srv.ListenAndServe()
}

func Shutdown(ctx context.Context, srv *http.Server) error {
	if srv == nil {
		return nil
	}
	return srv.Shutdown(ctx)
}
