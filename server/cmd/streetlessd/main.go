package main

import (
	"context"
	"crypto/tls"
	"errors"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"streetless/internal/store"
	"streetless/internal/web"
)

func main() {
	if err := run(); err != nil {
		log.Fatal(err)
	}
}

func run() error {
	cfg, err := web.LoadConfig(os.Getenv)
	if err != nil {
		return err
	}
	var tlsConfig *tls.Config
	if cfg.Transport == "direct" {
		pair, err := tls.LoadX509KeyPair(cfg.CertFile, cfg.KeyFile)
		if err != nil {
			return errors.New("invalid or unavailable STREETLESS_TLS_CERT or STREETLESS_TLS_KEY")
		}
		tlsConfig = &tls.Config{MinVersion: tls.VersionTLS12, Certificates: []tls.Certificate{pair}}
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	db, err := store.Open(cfg.DB, nil)
	if err != nil {
		return errors.New("invalid or unavailable STREETLESS_DB (check schema, path and permissions)")
	}
	defer db.Close()
	if err := db.Clean(ctx); err != nil {
		return errors.New("could not clean STREETLESS_DB at startup")
	}

	listener, err := net.Listen("tcp4", cfg.Listen)
	if err != nil {
		return errors.New("invalid or unavailable STREETLESS_LISTEN")
	}
	defer listener.Close()
	protocols := new(http.Protocols)
	protocols.SetHTTP1(true)
	srv := &http.Server{
		Handler:           web.New(db, cfg),
		TLSConfig:         tlsConfig,
		Protocols:         protocols,
		MaxHeaderBytes:    16 << 10,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       20 * time.Second,
		WriteTimeout:      20 * time.Second,
		IdleTimeout:       30 * time.Second,
	}

	cleanCtx, cancelClean := context.WithCancel(ctx)
	cleanDone := make(chan struct{})
	go func() {
		defer close(cleanDone)
		ticker := time.NewTicker(time.Minute)
		defer ticker.Stop()
		for {
			select {
			case <-cleanCtx.Done():
				return
			case <-ticker.C:
				if err := db.Clean(cleanCtx); err != nil && cleanCtx.Err() == nil {
					log.Print("store_cleanup_failed")
				}
			}
		}
	}()
	defer func() {
		cancelClean()
		<-cleanDone
	}()

	serveDone := make(chan error, 1)
	go func() {
		if cfg.Transport == "direct" {
			serveDone <- srv.ServeTLS(listener, "", "")
		} else {
			serveDone <- srv.Serve(listener)
		}
	}()
	select {
	case <-ctx.Done():
		drain, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if err := srv.Shutdown(drain); err != nil {
			_ = srv.Close()
			return errors.New("HTTP shutdown deadline exceeded")
		}
	case err := <-serveDone:
		if !errors.Is(err, http.ErrServerClosed) {
			_ = srv.Close()
			return errors.New("HTTP listener stopped unexpectedly")
		}
	}
	return nil
}
