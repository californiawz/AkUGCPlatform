package registry

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"regexp"
	"slices"
	"strings"
)

var subjectPattern = regexp.MustCompile(`^[a-zA-Z0-9][a-zA-Z0-9._:-]{0,127}$`)

type Authenticator interface {
	Authenticate(authorization string) (Principal, error)
}

type StaticAuthenticator struct {
	byTokenDigest map[[sha256.Size]byte]Principal
}

type AuthConfig struct {
	Principals []AuthPrincipalConfig `json:"principals"`
}

type AuthPrincipalConfig struct {
	Subject       string   `json:"subject"`
	TokenSHA256   string   `json:"tokenSha256"`
	Roles         []Role   `json:"roles"`
	PublisherKeys []string `json:"publisherKeys"`
}

func LoadStaticAuthenticator(path string) (*StaticAuthenticator, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read auth config: %w", err)
	}
	var config AuthConfig
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&config); err != nil {
		return nil, fmt.Errorf("decode auth config: %w", err)
	}
	if err := requireJSONEOF(decoder); err != nil {
		return nil, fmt.Errorf("decode auth config: trailing data")
	}
	return NewStaticAuthenticator(config)
}

func NewStaticAuthenticator(config AuthConfig) (*StaticAuthenticator, error) {
	if len(config.Principals) == 0 {
		return nil, fmt.Errorf("auth config must contain principals")
	}
	result := &StaticAuthenticator{
		byTokenDigest: make(map[[sha256.Size]byte]Principal, len(config.Principals)),
	}
	subjects := make(map[string]struct{}, len(config.Principals))
	for _, configured := range config.Principals {
		if !subjectPattern.MatchString(configured.Subject) {
			return nil, fmt.Errorf("invalid auth subject %q", configured.Subject)
		}
		if _, exists := subjects[configured.Subject]; exists {
			return nil, fmt.Errorf("duplicate auth subject %q", configured.Subject)
		}
		subjects[configured.Subject] = struct{}{}
		rawDigest, err := decodeLowerHex(configured.TokenSHA256, sha256.Size)
		if err != nil {
			return nil, fmt.Errorf("subject %q tokenSha256: %w", configured.Subject, err)
		}
		var digest [sha256.Size]byte
		copy(digest[:], rawDigest)
		if _, exists := result.byTokenDigest[digest]; exists {
			return nil, fmt.Errorf("duplicate token digest")
		}
		roles, err := validateRoles(configured.Roles)
		if err != nil {
			return nil, fmt.Errorf("subject %q: %w", configured.Subject, err)
		}
		keys := make([]string, 0, len(configured.PublisherKeys))
		seenKeys := make(map[string]struct{}, len(configured.PublisherKeys))
		for _, key := range configured.PublisherKeys {
			if _, err := decodeLowerHex(key, 32); err != nil {
				return nil, fmt.Errorf("subject %q publisher key: %w", configured.Subject, err)
			}
			if _, exists := seenKeys[key]; exists {
				return nil, fmt.Errorf("subject %q has duplicate publisher key", configured.Subject)
			}
			seenKeys[key] = struct{}{}
			keys = append(keys, key)
		}
		if slices.Contains(roles, RoleCreator) && len(keys) == 0 {
			return nil, fmt.Errorf("creator %q must have at least one publisher key", configured.Subject)
		}
		result.byTokenDigest[digest] = Principal{
			Subject:       configured.Subject,
			Roles:         roles,
			PublisherKeys: keys,
		}
	}
	return result, nil
}

func (a *StaticAuthenticator) Authenticate(authorization string) (Principal, error) {
	const prefix = "Bearer "
	if !strings.HasPrefix(authorization, prefix) {
		return Principal{}, ErrUnauthenticated
	}
	token := strings.TrimSpace(strings.TrimPrefix(authorization, prefix))
	if token == "" || len(token) > 4096 {
		return Principal{}, ErrUnauthenticated
	}
	digest := sha256.Sum256([]byte(token))
	principal, exists := a.byTokenDigest[digest]
	if !exists {
		return Principal{}, ErrUnauthenticated
	}
	return principal, nil
}

func validateRoles(roles []Role) ([]Role, error) {
	if len(roles) == 0 {
		return nil, fmt.Errorf("principal must have roles")
	}
	seen := make(map[Role]struct{}, len(roles))
	result := make([]Role, 0, len(roles))
	for _, role := range roles {
		switch role {
		case RoleCreator, RoleReviewer, RolePublisher, RolePlayer:
		default:
			return nil, fmt.Errorf("unknown role %q", role)
		}
		if _, exists := seen[role]; exists {
			return nil, fmt.Errorf("duplicate role %q", role)
		}
		seen[role] = struct{}{}
		result = append(result, role)
	}
	return result, nil
}

func TokenSHA256(token string) string {
	digest := sha256.Sum256([]byte(token))
	return hex.EncodeToString(digest[:])
}
