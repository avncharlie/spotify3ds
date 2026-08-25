package setupqr

import (
	"encoding/binary"
	"errors"
	"hash/crc32"
)

const (
	Version         = 1
	MaxClientID     = 127
	MaxRefreshToken = 255
	headerSize      = 9
	checksumSize    = 4
)

var magic = [4]byte{'S', 'P', '3', 'D'}

type Credentials struct {
	ClientID     string
	RefreshToken string
}

func Encode(credentials Credentials) ([]byte, error) {
	client := []byte(credentials.ClientID)
	refresh := []byte(credentials.RefreshToken)
	if len(client) == 0 || len(client) > MaxClientID {
		return nil, errors.New("client ID length is unsupported")
	}
	if len(refresh) == 0 || len(refresh) > MaxRefreshToken {
		return nil, errors.New("refresh token length is unsupported")
	}
	if !safe(client) || !safe(refresh) {
		return nil, errors.New("credentials contain unsupported control characters")
	}
	payload := make([]byte, headerSize+len(client)+len(refresh)+checksumSize)
	copy(payload[:4], magic[:])
	payload[4] = Version
	payload[5] = 0
	payload[6] = byte(len(client))
	binary.BigEndian.PutUint16(payload[7:9], uint16(len(refresh)))
	copy(payload[9:], client)
	copy(payload[9+len(client):], refresh)
	checksumAt := len(payload) - checksumSize
	binary.BigEndian.PutUint32(payload[checksumAt:], crc32.ChecksumIEEE(payload[:checksumAt]))
	return payload, nil
}

func Decode(payload []byte) (Credentials, error) {
	if len(payload) < headerSize+checksumSize {
		return Credentials{}, errors.New("payload is truncated")
	}
	if string(payload[:4]) != string(magic[:]) {
		return Credentials{}, errors.New("payload magic is invalid")
	}
	if payload[4] != Version || payload[5] != 0 {
		return Credentials{}, errors.New("payload version or flags are unsupported")
	}
	clientLen := int(payload[6])
	refreshLen := int(binary.BigEndian.Uint16(payload[7:9]))
	if clientLen == 0 || clientLen > MaxClientID || refreshLen == 0 || refreshLen > MaxRefreshToken {
		return Credentials{}, errors.New("credential lengths are invalid")
	}
	want := headerSize + clientLen + refreshLen + checksumSize
	if len(payload) != want {
		return Credentials{}, errors.New("payload length does not match its header")
	}
	checksumAt := want - checksumSize
	if binary.BigEndian.Uint32(payload[checksumAt:]) != crc32.ChecksumIEEE(payload[:checksumAt]) {
		return Credentials{}, errors.New("payload checksum is invalid")
	}
	client := payload[headerSize : headerSize+clientLen]
	refresh := payload[headerSize+clientLen : checksumAt]
	if !safe(client) || !safe(refresh) {
		return Credentials{}, errors.New("credentials contain unsupported control characters")
	}
	return Credentials{ClientID: string(client), RefreshToken: string(refresh)}, nil
}

func safe(value []byte) bool {
	for _, b := range value {
		if b < 0x21 || b > 0x7e {
			return false
		}
	}
	return true
}
