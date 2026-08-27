package main

import (
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"reflect"
	"strings"
	"testing"
	"time"
)

func TestRuntimeRootInventory(t *testing.T) {
	repoRoot, err := findRepoRoot()
	if err != nil {
		t.Fatal(err)
	}

	names, err := runtimeRootNames(repoRoot)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{
		"digicert_g2",
		"digicert_g3",
		"globalsign_r3",
		"gts_root_r4",
		"starfield_g2",
	}
	if !reflect.DeepEqual(names, want) {
		t.Fatalf("runtime roots = %v, want %v", names, want)
	}

	roots, err := loadRuntimeRoots(repoRoot, time.Now().UTC(), 14*24*time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	if len(roots.certs) != len(want) {
		t.Fatalf("parsed roots = %d, want %d", len(roots.certs), len(want))
	}
}

func TestKnownHostInventory(t *testing.T) {
	want := []string{
		"api.spotify.com",
		"accounts.spotify.com",
		"i.scdn.co",
		"mosaic.scdn.co",
		"image-cdn-fa.spotifycdn.com",
		"image-cdn-ak.spotifycdn.com",
		"lrclib.net",
	}
	if !reflect.DeepEqual(hosts, want) {
		t.Fatalf("hosts = %v, want %v", hosts, want)
	}
}

func TestTLSConfigMatchesConsole(t *testing.T) {
	config := tlsConfig("api.spotify.com", x509.NewCertPool())
	if config.ServerName != "api.spotify.com" {
		t.Fatalf("ServerName = %q", config.ServerName)
	}
	if config.MinVersion != tls.VersionTLS12 || config.MaxVersion != tls.VersionTLS12 {
		t.Fatalf("TLS versions = %x..%x, want TLS 1.2 only", config.MinVersion, config.MaxVersion)
	}
}

func TestInspectConnection(t *testing.T) {
	now := time.Date(2026, time.August, 27, 12, 0, 0, 0, time.UTC)
	minValidity := 14 * 24 * time.Hour

	newState := func() (tls.ConnectionState, map[[sha256.Size]byte]string) {
		leaf := &x509.Certificate{
			Raw:       []byte("leaf"),
			DNSNames:  []string{"api.spotify.com"},
			NotBefore: now.Add(-time.Hour),
			NotAfter:  now.Add(15 * 24 * time.Hour),
		}
		leaf.Issuer.CommonName = "Test Intermediate"
		root := &x509.Certificate{
			Raw:       []byte("root"),
			NotBefore: now.Add(-time.Hour),
			NotAfter:  now.Add(365 * 24 * time.Hour),
		}
		root.Subject.CommonName = "Test Root"
		rootHash := sha256.Sum256(root.Raw)
		return tls.ConnectionState{
			Version:          tls.VersionTLS12,
			CipherSuite:      tls.TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
			PeerCertificates: []*x509.Certificate{leaf, root},
			VerifiedChains:   [][]*x509.Certificate{{leaf, root}},
		}, map[[sha256.Size]byte]string{rootHash: "test_root.der"}
	}

	t.Run("valid", func(t *testing.T) {
		state, roots := newState()
		report, err := inspectConnection("api.spotify.com", state, roots, now, minValidity)
		if err != nil {
			t.Fatal(err)
		}
		if report.anchor != "test_root.der" || report.issuer != "Test Intermediate" {
			t.Fatalf("unexpected report: %+v", report)
		}
	})

	t.Run("hostname mismatch", func(t *testing.T) {
		state, roots := newState()
		_, err := inspectConnection("accounts.spotify.com", state, roots, now, minValidity)
		if err == nil || !strings.Contains(err.Error(), "hostname verification") {
			t.Fatalf("error = %v", err)
		}
	})

	t.Run("leaf expiry runway", func(t *testing.T) {
		state, roots := newState()
		state.PeerCertificates[0].NotAfter = now.Add(13 * 24 * time.Hour)
		_, err := inspectConnection("api.spotify.com", state, roots, now, minValidity)
		if err == nil || !strings.Contains(err.Error(), "before the required") {
			t.Fatalf("error = %v", err)
		}
	})

	t.Run("unknown anchor", func(t *testing.T) {
		state, _ := newState()
		_, err := inspectConnection("api.spotify.com", state, nil, now, minValidity)
		if err == nil || !strings.Contains(err.Error(), "unrecognized root") {
			t.Fatalf("error = %v", err)
		}
	})

	t.Run("wrong TLS version", func(t *testing.T) {
		state, roots := newState()
		state.Version = tls.VersionTLS13
		_, err := inspectConnection("api.spotify.com", state, roots, now, minValidity)
		if err == nil || !strings.Contains(err.Error(), "instead of TLS 1.2") {
			t.Fatalf("error = %v", err)
		}
	})
}
