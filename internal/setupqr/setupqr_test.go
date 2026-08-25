package setupqr

import "testing"

func TestRoundTrip(t *testing.T) {
	want := Credentials{
		ClientID:     "0123456789abcdef0123456789abcdef",
		RefreshToken: "AQB-test_refresh-token_123",
	}
	payload, err := Encode(want)
	if err != nil {
		t.Fatal(err)
	}
	got, err := Decode(payload)
	if err != nil {
		t.Fatal(err)
	}
	if got != want {
		t.Fatalf("got %#v, want %#v", got, want)
	}
	payload[len(payload)-1] ^= 1
	if _, err := Decode(payload); err == nil {
		t.Fatal("corrupt payload was accepted")
	}
}

func TestLimits(t *testing.T) {
	client := make([]byte, MaxClientID)
	refresh := make([]byte, MaxRefreshToken)
	for i := range client {
		client[i] = 'a'
	}
	for i := range refresh {
		refresh[i] = 'b'
	}
	payload, err := Encode(Credentials{string(client), string(refresh)})
	if err != nil {
		t.Fatal(err)
	}
	if len(payload) != 395 {
		t.Fatalf("maximum payload is %d bytes, want 395", len(payload))
	}
}
