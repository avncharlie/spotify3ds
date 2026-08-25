package setupoauth

import (
	"strings"
	"testing"
)

func TestRequiredScopesRemainComplete(t *testing.T) {
	want := []string{
		"user-read-playback-state", "user-modify-playback-state",
		"user-read-currently-playing", "user-read-recently-played",
		"user-library-read", "playlist-read-private", "user-top-read",
	}
	for _, scope := range want {
		found := false
		for _, actual := range strings.Fields(Scopes) {
			if actual == scope {
				found = true
				break
			}
		}
		if !found {
			t.Fatalf("required scope %q was removed", scope)
		}
	}
}
