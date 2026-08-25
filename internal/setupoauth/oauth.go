package setupoauth

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/avncharlie/spotify3ds/internal/setupqr"
)

const (
	RedirectURI = "http://127.0.0.1:8888/callback"
	Scopes      = "user-read-playback-state user-modify-playback-state user-read-currently-playing user-read-recently-played user-library-read playlist-read-private user-top-read"
)

type Result struct {
	Credentials setupqr.Credentials
	DeviceCount int
}

type callbackResult struct {
	Code  string
	State string
	Error string
}

type tokenResponse struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	Scope        string `json:"scope"`
	Error        string `json:"error"`
	Description  string `json:"error_description"`
}

func Authorize(ctx context.Context, clientID string, openBrowser func(string) error,
	status func(string)) (Result, error) {
	clientID = strings.TrimSpace(clientID)
	if clientID == "" || len(clientID) > setupqr.MaxClientID {
		return Result{}, errors.New("enter a valid Spotify Client ID")
	}
	listener, err := net.Listen("tcp4", "127.0.0.1:8888")
	if err != nil {
		return Result{}, fmt.Errorf("callback port 8888 is unavailable: %w", err)
	}
	defer listener.Close()

	verifier, err := randomURLSafe(64)
	if err != nil {
		return Result{}, err
	}
	state, err := randomURLSafe(16)
	if err != nil {
		return Result{}, err
	}
	challengeRaw := sha256.Sum256([]byte(verifier))
	challenge := base64.RawURLEncoding.EncodeToString(challengeRaw[:])
	callback := make(chan callbackResult, 1)
	mux := http.NewServeMux()
	mux.HandleFunc("/callback", func(w http.ResponseWriter, r *http.Request) {
		result := callbackResult{
			Code:  r.URL.Query().Get("code"),
			State: r.URL.Query().Get("state"),
			Error: r.URL.Query().Get("error"),
		}
		select {
		case callback <- result:
		default:
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'")
		io.WriteString(w, "<html><body style='font-family:system-ui;text-align:center;padding-top:4em'><h2>Spotify3DS authorization received</h2><p>You can close this tab and return to the setup app.</p></body></html>")
	})
	server := &http.Server{Handler: mux, ReadHeaderTimeout: 5 * time.Second}
	go func() { _ = server.Serve(listener) }()
	defer server.Shutdown(context.Background())

	authorizeURL := "https://accounts.spotify.com/authorize?" + url.Values{
		"client_id": {clientID}, "response_type": {"code"},
		"redirect_uri": {RedirectURI}, "scope": {Scopes},
		"code_challenge_method": {"S256"}, "code_challenge": {challenge},
		"state": {state},
	}.Encode()
	status("Waiting for Spotify authorization in your browser...")
	if err := openBrowser(authorizeURL); err != nil {
		return Result{}, fmt.Errorf("open browser: %w", err)
	}

	var received callbackResult
	select {
	case <-ctx.Done():
		return Result{}, errors.New("authorization timed out or was cancelled")
	case received = <-callback:
	}
	if received.Error != "" {
		return Result{}, fmt.Errorf("Spotify authorization denied: %s", received.Error)
	}
	if received.State != state || received.Code == "" {
		return Result{}, errors.New("Spotify returned an invalid authorization callback")
	}

	status("Exchanging the authorization code...")
	tokens, err := postToken(ctx, url.Values{
		"grant_type": {"authorization_code"}, "code": {received.Code},
		"redirect_uri": {RedirectURI}, "client_id": {clientID},
		"code_verifier": {verifier},
	})
	if err != nil {
		return Result{}, err
	}
	if tokens.RefreshToken == "" {
		return Result{}, errors.New("Spotify did not return a refresh token")
	}
	if err := requireScopes(tokens.Scope); err != nil {
		return Result{}, err
	}

	status("Validating refresh and Spotify API access...")
	refreshed, err := postToken(ctx, url.Values{
		"grant_type": {"refresh_token"}, "refresh_token": {tokens.RefreshToken},
		"client_id": {clientID},
	})
	if err != nil {
		return Result{}, fmt.Errorf("refresh-token validation failed: %w", err)
	}
	refresh := tokens.RefreshToken
	if refreshed.RefreshToken != "" {
		refresh = refreshed.RefreshToken
	}
	access := refreshed.AccessToken
	if access == "" {
		access = tokens.AccessToken
	}
	if access == "" {
		return Result{}, errors.New("Spotify did not return an access token")
	}
	devices, err := deviceCount(ctx, access)
	if err != nil {
		return Result{}, fmt.Errorf("Spotify API validation failed: %w", err)
	}
	credentials := setupqr.Credentials{ClientID: clientID, RefreshToken: refresh}
	if _, err := setupqr.Encode(credentials); err != nil {
		return Result{}, err
	}
	return Result{Credentials: credentials, DeviceCount: devices}, nil
}

func randomURLSafe(bytes int) (string, error) {
	value := make([]byte, bytes)
	if _, err := rand.Read(value); err != nil {
		return "", fmt.Errorf("secure random generation failed: %w", err)
	}
	return base64.RawURLEncoding.EncodeToString(value), nil
}

func postToken(ctx context.Context, values url.Values) (tokenResponse, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodPost,
		"https://accounts.spotify.com/api/token", strings.NewReader(values.Encode()))
	if err != nil {
		return tokenResponse{}, err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return tokenResponse{}, err
	}
	defer resp.Body.Close()
	var result tokenResponse
	if err := json.NewDecoder(io.LimitReader(resp.Body, 1<<20)).Decode(&result); err != nil {
		return tokenResponse{}, errors.New("Spotify returned an unreadable token response")
	}
	if resp.StatusCode/100 != 2 {
		message := result.Error
		if result.Description != "" {
			message += ": " + result.Description
		}
		return tokenResponse{}, fmt.Errorf("Spotify token request failed (%d): %s", resp.StatusCode, message)
	}
	return result, nil
}

func requireScopes(granted string) error {
	have := map[string]bool{}
	for _, scope := range strings.Fields(granted) {
		have[scope] = true
	}
	for _, scope := range strings.Fields(Scopes) {
		if !have[scope] {
			return fmt.Errorf("Spotify did not grant required scope %q", scope)
		}
	}
	return nil
}

func deviceCount(ctx context.Context, access string) (int, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet,
		"https://api.spotify.com/v1/me/player/devices", nil)
	if err != nil {
		return 0, err
	}
	req.Header.Set("Authorization", "Bearer "+access)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return 0, fmt.Errorf("devices endpoint returned HTTP %d", resp.StatusCode)
	}
	var body struct {
		Devices []json.RawMessage `json:"devices"`
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, 1<<20)).Decode(&body); err != nil {
		return 0, err
	}
	return len(body.Devices), nil
}
