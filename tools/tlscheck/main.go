package main

import (
	"context"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

const (
	defaultMinValidDays = 14
	defaultTimeout      = 15 * time.Second
	maxAttempts         = 3
)

var hosts = []string{
	"api.spotify.com",
	"accounts.spotify.com",
	"i.scdn.co",
	"mosaic.scdn.co",
	"image-cdn-fa.spotifycdn.com",
	"image-cdn-ak.spotifycdn.com",
	"lrclib.net",
}

var rootPairPattern = regexp.MustCompile(`\{\s*([a-z][a-z0-9_]*)_der\s*,\s*([a-z][a-z0-9_]*)_der_end\s*\}`)

type rootSet struct {
	pool         *x509.CertPool
	certs        []*x509.Certificate
	nameBySHA256 map[[sha256.Size]byte]string
}

type hostReport struct {
	host       string
	version    uint16
	cipher     uint16
	issuer     string
	anchor     string
	anchorHash [sha256.Size]byte
	leafExpiry time.Time
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run() error {
	repoRoot, err := findRepoRoot()
	if err != nil {
		return err
	}

	minValidDays, err := positiveIntEnv("TLS_MIN_VALID_DAYS", defaultMinValidDays)
	if err != nil {
		return err
	}
	minValidity := time.Duration(minValidDays) * 24 * time.Hour
	now := time.Now().UTC()

	roots, err := loadRuntimeRoots(repoRoot, now, minValidity)
	if err != nil {
		return fmt.Errorf("load embedded roots: %w", err)
	}

	fmt.Printf("Loaded %d runtime roots; requiring at least %d days of validity\n", len(roots.certs), minValidDays)
	for _, cert := range roots.certs {
		fingerprint := sha256.Sum256(cert.Raw)
		fmt.Printf("root %-24s expires %s  sha256 %s\n",
			roots.nameBySHA256[fingerprint], cert.NotAfter.UTC().Format(time.DateOnly), formatFingerprint(fingerprint))
	}

	var failures []error
	for _, host := range hosts {
		report, err := checkHostWithRetries(host, roots, now, minValidity)
		if err != nil {
			failures = append(failures, fmt.Errorf("%s: %w", host, err))
			fmt.Fprintf(os.Stderr, "%s: FAILED: %v\n", host, err)
			continue
		}

		remaining := report.leafExpiry.Sub(now).Round(time.Hour)
		fmt.Printf("%-36s OK  %s  %s  issuer=%q  anchor=%s  leaf-expires=%s (%s)  anchor-sha256=%s\n",
			report.host,
			tls.VersionName(report.version),
			tls.CipherSuiteName(report.cipher),
			report.issuer,
			report.anchor,
			report.leafExpiry.UTC().Format(time.RFC3339),
			remaining,
			formatFingerprint(report.anchorHash))
	}

	if len(failures) != 0 {
		return fmt.Errorf("%d of %d live TLS checks failed", len(failures), len(hosts))
	}

	fmt.Printf("All %d live TLS checks passed\n", len(hosts))
	return nil
}

func findRepoRoot() (string, error) {
	dir, err := os.Getwd()
	if err != nil {
		return "", fmt.Errorf("get working directory: %w", err)
	}

	for {
		if fileExists(filepath.Join(dir, "go.mod")) && fileExists(filepath.Join(dir, "source", "net", "tls.c")) {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", errors.New("repository root not found")
		}
		dir = parent
	}
}

func fileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && !info.IsDir()
}

func positiveIntEnv(name string, fallback int) (int, error) {
	value := os.Getenv(name)
	if value == "" {
		return fallback, nil
	}

	parsed, err := strconv.Atoi(value)
	if err != nil || parsed <= 0 {
		return 0, fmt.Errorf("%s must be a positive integer, got %q", name, value)
	}
	return parsed, nil
}

func runtimeRootNames(repoRoot string) ([]string, error) {
	sourcePath := filepath.Join(repoRoot, "source", "net", "tls.c")
	source, err := os.ReadFile(sourcePath)
	if err != nil {
		return nil, fmt.Errorf("read %s: %w", sourcePath, err)
	}

	matches := rootPairPattern.FindAllSubmatch(source, -1)
	if len(matches) == 0 {
		return nil, errors.New("no runtime root pairs found in source/net/tls.c")
	}

	seen := make(map[string]bool, len(matches))
	names := make([]string, 0, len(matches))
	for _, match := range matches {
		beginName := string(match[1])
		endName := string(match[2])
		if beginName != endName {
			return nil, fmt.Errorf("mismatched runtime root symbols %s_der and %s_der_end", beginName, endName)
		}
		if seen[beginName] {
			return nil, fmt.Errorf("duplicate runtime root %s", beginName)
		}
		seen[beginName] = true
		names = append(names, beginName)
	}
	return names, nil
}

func loadRuntimeRoots(repoRoot string, now time.Time, minValidity time.Duration) (*rootSet, error) {
	names, err := runtimeRootNames(repoRoot)
	if err != nil {
		return nil, err
	}

	roots := &rootSet{
		pool:         x509.NewCertPool(),
		nameBySHA256: make(map[[sha256.Size]byte]string, len(names)),
	}
	for _, name := range names {
		path := filepath.Join(repoRoot, "data", name+".der")
		der, err := os.ReadFile(path)
		if err != nil {
			return nil, fmt.Errorf("read runtime root %s: %w", path, err)
		}
		cert, err := x509.ParseCertificate(der)
		if err != nil {
			return nil, fmt.Errorf("parse runtime root %s: %w", path, err)
		}
		if !cert.IsCA {
			return nil, fmt.Errorf("runtime root %s is not a CA certificate", path)
		}
		if err := cert.CheckSignatureFrom(cert); err != nil {
			return nil, fmt.Errorf("runtime root %s is not self-signed: %w", path, err)
		}
		if err := checkCertificateValidity(cert, now, minValidity); err != nil {
			return nil, fmt.Errorf("runtime root %s: %w", path, err)
		}

		fingerprint := sha256.Sum256(cert.Raw)
		if previous, exists := roots.nameBySHA256[fingerprint]; exists {
			return nil, fmt.Errorf("runtime roots %s and %s contain the same certificate", previous, name+".der")
		}
		roots.pool.AddCert(cert)
		roots.certs = append(roots.certs, cert)
		roots.nameBySHA256[fingerprint] = name + ".der"
	}
	return roots, nil
}

func checkHostWithRetries(host string, roots *rootSet, now time.Time, minValidity time.Duration) (*hostReport, error) {
	var lastErr error
	for attempt := 1; attempt <= maxAttempts; attempt++ {
		report, retryable, err := checkHost(host, roots, now, minValidity, defaultTimeout)
		if err == nil {
			return report, nil
		}
		lastErr = err
		if !retryable || attempt == maxAttempts {
			break
		}
		fmt.Fprintf(os.Stderr, "%s: attempt %d/%d failed: %v; retrying\n", host, attempt, maxAttempts, err)
		time.Sleep(time.Duration(attempt) * time.Second)
	}
	return nil, lastErr
}

func checkHost(host string, roots *rootSet, now time.Time, minValidity, timeout time.Duration) (*hostReport, bool, error) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	raw, err := (&net.Dialer{}).DialContext(ctx, "tcp", net.JoinHostPort(host, "443"))
	if err != nil {
		return nil, true, fmt.Errorf("connect: %w", err)
	}
	defer raw.Close()

	conn := tls.Client(raw, tlsConfig(host, roots.pool))
	if err := conn.HandshakeContext(ctx); err != nil {
		return nil, !isCertificateVerificationError(err), fmt.Errorf("TLS handshake: %w", err)
	}
	defer conn.Close()

	report, err := inspectConnection(host, conn.ConnectionState(), roots.nameBySHA256, now, minValidity)
	if err != nil {
		return nil, false, err
	}
	return report, false, nil
}

func tlsConfig(host string, roots *x509.CertPool) *tls.Config {
	return &tls.Config{
		RootCAs:    roots,
		ServerName: host,
		MinVersion: tls.VersionTLS12,
		MaxVersion: tls.VersionTLS12,
	}
}

func inspectConnection(host string, state tls.ConnectionState, rootNames map[[sha256.Size]byte]string, now time.Time, minValidity time.Duration) (*hostReport, error) {
	if state.Version != tls.VersionTLS12 {
		return nil, fmt.Errorf("negotiated %s instead of TLS 1.2", tls.VersionName(state.Version))
	}
	if len(state.PeerCertificates) == 0 {
		return nil, errors.New("server returned no leaf certificate")
	}
	leaf := state.PeerCertificates[0]
	if err := leaf.VerifyHostname(host); err != nil {
		return nil, fmt.Errorf("hostname verification: %w", err)
	}
	if len(state.VerifiedChains) == 0 || len(state.VerifiedChains[0]) == 0 {
		return nil, errors.New("TLS handshake produced no verified chain")
	}

	chain := state.VerifiedChains[0]
	for index, cert := range chain {
		if err := checkCertificateValidity(cert, now, minValidity); err != nil {
			return nil, fmt.Errorf("verified chain certificate %d (%s): %w", index, certificateName(cert), err)
		}
	}

	anchorHash := sha256.Sum256(chain[len(chain)-1].Raw)
	anchorName, ok := rootNames[anchorHash]
	if !ok {
		return nil, fmt.Errorf("verified chain terminates at an unrecognized root with SHA-256 %s", formatFingerprint(anchorHash))
	}

	return &hostReport{
		host:       host,
		version:    state.Version,
		cipher:     state.CipherSuite,
		issuer:     certificateNameFromPKIX(leaf.Issuer.CommonName, leaf.Issuer.String()),
		anchor:     anchorName,
		anchorHash: anchorHash,
		leafExpiry: leaf.NotAfter,
	}, nil
}

func checkCertificateValidity(cert *x509.Certificate, now time.Time, minValidity time.Duration) error {
	if now.Before(cert.NotBefore) {
		return fmt.Errorf("not valid before %s", cert.NotBefore.UTC().Format(time.RFC3339))
	}
	deadline := now.Add(minValidity)
	if !cert.NotAfter.After(deadline) {
		return fmt.Errorf("expires at %s, before the required %s runway",
			cert.NotAfter.UTC().Format(time.RFC3339), minValidity.Round(time.Hour))
	}
	return nil
}

func isCertificateVerificationError(err error) bool {
	var verifyErr *tls.CertificateVerificationError
	if errors.As(err, &verifyErr) {
		return true
	}
	var hostnameErr x509.HostnameError
	if errors.As(err, &hostnameErr) {
		return true
	}
	var authorityErr x509.UnknownAuthorityError
	if errors.As(err, &authorityErr) {
		return true
	}
	var invalidErr x509.CertificateInvalidError
	return errors.As(err, &invalidErr)
}

func certificateName(cert *x509.Certificate) string {
	return certificateNameFromPKIX(cert.Subject.CommonName, cert.Subject.String())
}

func certificateNameFromPKIX(commonName, fallback string) string {
	if commonName != "" {
		return commonName
	}
	return fallback
}

func formatFingerprint(fingerprint [sha256.Size]byte) string {
	encoded := strings.ToUpper(hex.EncodeToString(fingerprint[:]))
	parts := make([]string, 0, sha256.Size)
	for offset := 0; offset < len(encoded); offset += 2 {
		parts = append(parts, encoded[offset:offset+2])
	}
	return strings.Join(parts, ":")
}
