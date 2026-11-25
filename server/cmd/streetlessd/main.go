package main

import (
	"context"
	"crypto/rand"
	"crypto/tls"
	"errors"
	"fmt"
	"log"
	"math/big"
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
	if len(os.Args) == 3 && os.Args[1] == "--bootstrap-admin" {
		return bootstrapAdmin(os.Args[2])
	}
	debugFlag := false
	for _, arg := range os.Args[1:] {
		if arg != "--debug" {
			return fmt.Errorf("unknown argument %q", arg)
		}
		debugFlag = true
	}
	getenv := func(key string) string {
		if key == "STREETLESS_DEBUG" && debugFlag {
			return "1"
		}
		return os.Getenv(key)
	}
	cfg, err := web.LoadConfig(getenv)
	if err != nil {
		return err
	}
	if cfg.Debug {
		pin, err := rand.Int(rand.Reader, big.NewInt(10000))
		if err != nil {
			return errors.New("could not generate debug PIN")
		}
		cfg.DebugPIN = fmt.Sprintf("%04d", pin.Int64())
		log.Printf("DEBUG ONLY: isolated non-production HTTP; do not use production credentials. PIN: %s", cfg.DebugPIN)
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

// bootstrapAdmin is an offline, operator-authorized recovery for databases
// created before participant accounts could have administrator roles.
func bootstrapAdmin(username string) error {
	path := os.Getenv("STREETLESS_DB")
	if path == "" {
		path = "streetless.sqlite"
	}
	info, err := os.Stat(path)
	if err != nil || !info.Mode().IsRegular() {
		return errors.New("STREETLESS_DB must refer to an existing database")
	}
	db, err := store.Open(path, nil)
	if err != nil {
		return errors.New("invalid or unavailable STREETLESS_DB (check schema, path and permissions)")
	}
	defer db.Close()
	if err := db.BootstrapAdmin(context.Background(), username); err != nil {
		return fmt.Errorf("could not bootstrap administrator: %w", err)
	}
	log.Printf("Administrator role granted to %q", username)
	return nil
}
