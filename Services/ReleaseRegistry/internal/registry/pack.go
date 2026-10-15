package registry

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strconv"
	"strings"
	"unicode/utf8"
)

const (
	maxJSONDepth    = 32
	maxJSONTokens   = 131072
	maxJSONKeyRunes = 128
	maxCapabilities = 16
	maxDependencies = 1024
)

type packManifest struct {
	ReleaseID         string   `json:"releaseId"`
	SchemaVersion     int32    `json:"schemaVersion"`
	ProjectID         string   `json:"projectId"`
	TemplateID        string   `json:"templateId"`
	Capabilities      []string `json:"capabilities"`
	AssetDependencies []string `json:"assetDependencies"`
	ContentHash       string   `json:"contentHash"`
}

type packSignature struct {
	Algorithm string `json:"algorithm"`
	PublicKey string `json:"publicKey"`
	Signature string `json:"signature"`
}

type packEnvelope struct {
	Manifest  packManifest    `json:"manifest"`
	Document  json.RawMessage `json:"document"`
	Program   json.RawMessage `json:"program"`
	Signature packSignature   `json:"signature"`
}

func InspectAndVerifyEnvelope(artifact []byte) (PackMetadata, error) {
	if len(artifact) == 0 || len(artifact) > MaxArtifactBytes {
		return PackMetadata{}, fmt.Errorf("%w: artifact size must be 1..%d bytes", ErrInvalidInput, MaxArtifactBytes)
	}
	if !utf8.Valid(artifact) {
		return PackMetadata{}, fmt.Errorf("%w: artifact is not valid UTF-8", ErrInvalidInput)
	}
	if err := validateJSONShape(artifact); err != nil {
		return PackMetadata{}, fmt.Errorf("%w: %v", ErrInvalidInput, err)
	}
	envelopeFields, err := requireExactObjectFields(
		artifact,
		"pack envelope",
		[]string{"manifest", "document", "program", "signature"},
	)
	if err != nil {
		return PackMetadata{}, fmt.Errorf("%w: %v", ErrInvalidInput, err)
	}
	if _, err := requireExactObjectFields(
		envelopeFields["manifest"],
		"manifest",
		[]string{
			"releaseId",
			"schemaVersion",
			"projectId",
			"templateId",
			"capabilities",
			"assetDependencies",
			"contentHash",
		},
	); err != nil {
		return PackMetadata{}, fmt.Errorf("%w: %v", ErrInvalidInput, err)
	}
	if _, err := requireExactObjectFields(
		envelopeFields["signature"],
		"signature",
		[]string{"algorithm", "publicKey", "signature"},
	); err != nil {
		return PackMetadata{}, fmt.Errorf("%w: %v", ErrInvalidInput, err)
	}

	var envelope packEnvelope
	decoder := json.NewDecoder(bytes.NewReader(artifact))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&envelope); err != nil {
		return PackMetadata{}, fmt.Errorf("%w: invalid pack envelope: %v", ErrInvalidInput, err)
	}
	if err := requireJSONEOF(decoder); err != nil {
		return PackMetadata{}, fmt.Errorf("%w: invalid trailing JSON data", ErrInvalidInput)
	}
	if !isJSONObject(envelope.Document) || !isJSONObject(envelope.Program) {
		return PackMetadata{}, fmt.Errorf("%w: document and program must be objects", ErrInvalidInput)
	}

	releaseID, err := canonicalUUID(envelope.Manifest.ReleaseID, true)
	if err != nil {
		return PackMetadata{}, fmt.Errorf("%w: manifest.releaseId: %v", ErrInvalidInput, err)
	}
	projectID, err := canonicalUUID(envelope.Manifest.ProjectID, false)
	if err != nil {
		return PackMetadata{}, fmt.Errorf("%w: manifest.projectId: %v", ErrInvalidInput, err)
	}
	if envelope.Manifest.SchemaVersion < 0 {
		return PackMetadata{}, fmt.Errorf("%w: schemaVersion must be non-negative", ErrInvalidInput)
	}
	if envelope.Manifest.TemplateID == "" || utf8.RuneCountInString(envelope.Manifest.TemplateID) > 128 {
		return PackMetadata{}, fmt.Errorf("%w: templateId length is invalid", ErrInvalidInput)
	}
	if err := validateStringList(envelope.Manifest.Capabilities, maxCapabilities, 64, false); err != nil {
		return PackMetadata{}, fmt.Errorf("%w: capabilities: %v", ErrInvalidInput, err)
	}
	if err := validateStringList(envelope.Manifest.AssetDependencies, maxDependencies, 256, true); err != nil {
		return PackMetadata{}, fmt.Errorf("%w: assetDependencies: %v", ErrInvalidInput, err)
	}
	if _, err := decodeLowerHex(envelope.Manifest.ContentHash, sha256.Size); err != nil {
		return PackMetadata{}, fmt.Errorf("%w: contentHash: %v", ErrInvalidInput, err)
	}
	if envelope.Signature.Algorithm != "ed25519" {
		return PackMetadata{}, fmt.Errorf("%w: signature algorithm must be ed25519", ErrInvalidInput)
	}
	publicKey, err := decodeLowerHex(envelope.Signature.PublicKey, ed25519.PublicKeySize)
	if err != nil {
		return PackMetadata{}, fmt.Errorf("%w: publicKey: %v", ErrInvalidInput, err)
	}
	signature, err := decodeLowerHex(envelope.Signature.Signature, ed25519.SignatureSize)
	if err != nil {
		return PackMetadata{}, fmt.Errorf("%w: signature: %v", ErrInvalidInput, err)
	}

	envelope.Manifest.ReleaseID = releaseID
	envelope.Manifest.ProjectID = projectID
	message := deterministicManifest(envelope.Manifest)
	if !ed25519.Verify(ed25519.PublicKey(publicKey), []byte(message), signature) {
		return PackMetadata{}, fmt.Errorf("%w: manifest signature is invalid", ErrInvalidInput)
	}

	digest := sha256.Sum256(artifact)
	return PackMetadata{
		ReleaseID:          releaseID,
		ProjectID:          projectID,
		SchemaVersion:      envelope.Manifest.SchemaVersion,
		ContentHash:        envelope.Manifest.ContentHash,
		PublisherPublicKey: envelope.Signature.PublicKey,
		ArtifactSHA256:     hex.EncodeToString(digest[:]),
		ArtifactSize:       int64(len(artifact)),
	}, nil
}

func deterministicManifest(manifest packManifest) string {
	var result strings.Builder
	result.Grow(256)
	result.WriteString(`{"assetdependencies":`)
	appendStringArray(&result, manifest.AssetDependencies)
	result.WriteString(`,"capabilities":`)
	appendStringArray(&result, manifest.Capabilities)
	result.WriteString(`,"contenthash":`)
	appendJSONString(&result, manifest.ContentHash)
	result.WriteString(`,"projectid":`)
	appendJSONString(&result, manifest.ProjectID)
	result.WriteString(`,"releaseid":`)
	appendJSONString(&result, manifest.ReleaseID)
	result.WriteString(`,"schemaversion":`)
	result.WriteString(strconv.FormatInt(int64(manifest.SchemaVersion), 10))
	result.WriteString(`,"templateid":`)
	appendJSONString(&result, manifest.TemplateID)
	result.WriteByte('}')
	return result.String()
}

func appendStringArray(result *strings.Builder, values []string) {
	result.WriteByte('[')
	for index, value := range values {
		if index > 0 {
			result.WriteByte(',')
		}
		appendJSONString(result, value)
	}
	result.WriteByte(']')
}

func appendJSONString(result *strings.Builder, value string) {
	result.WriteByte('"')
	for _, char := range value {
		switch char {
		case '"':
			result.WriteString(`\"`)
		case '\\':
			result.WriteString(`\\`)
		case '\b':
			result.WriteString(`\b`)
		case '\f':
			result.WriteString(`\f`)
		case '\n':
			result.WriteString(`\n`)
		case '\r':
			result.WriteString(`\r`)
		case '\t':
			result.WriteString(`\t`)
		default:
			switch {
			case char >= 0x20 && char <= 0x7e:
				result.WriteRune(char)
			case char <= 0xffff:
				fmt.Fprintf(result, `\u%04x`, char)
			default:
				adjusted := char - 0x10000
				fmt.Fprintf(result, `\u%04x\u%04x`, 0xd800+(adjusted>>10), 0xdc00+(adjusted&0x3ff))
			}
		}
	}
	result.WriteByte('"')
}

func validateStringList(values []string, maxCount, maxRunes int, allowEmpty bool) error {
	if values == nil || len(values) > maxCount {
		return fmt.Errorf("must contain at most %d strings", maxCount)
	}
	for _, value := range values {
		length := utf8.RuneCountInString(value)
		if (!allowEmpty && length == 0) || length > maxRunes {
			minimum := 1
			if allowEmpty {
				minimum = 0
			}
			return fmt.Errorf("item length must be %d..%d runes", minimum, maxRunes)
		}
	}
	return nil
}

func decodeLowerHex(value string, byteCount int) ([]byte, error) {
	if len(value) != byteCount*2 || value != strings.ToLower(value) {
		return nil, fmt.Errorf("must be %d lowercase hexadecimal characters", byteCount*2)
	}
	decoded, err := hex.DecodeString(value)
	if err != nil {
		return nil, fmt.Errorf("must be %d lowercase hexadecimal characters", byteCount*2)
	}
	return decoded, nil
}

func canonicalUUID(value string, requireNonZero bool) (string, error) {
	compact := value
	if len(value) == 36 {
		if value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-' {
			return "", errors.New("invalid UUID separators")
		}
		compact = strings.ReplaceAll(value, "-", "")
	}
	if len(compact) != 32 {
		return "", errors.New("must contain 32 hexadecimal digits")
	}
	if _, err := hex.DecodeString(compact); err != nil {
		return "", errors.New("must contain 32 hexadecimal digits")
	}
	compact = strings.ToLower(compact)
	if requireNonZero && compact == strings.Repeat("0", 32) {
		return "", errors.New("must not be zero")
	}
	return compact[0:8] + "-" + compact[8:12] + "-" + compact[12:16] + "-" +
		compact[16:20] + "-" + compact[20:32], nil
}

func isJSONObject(raw json.RawMessage) bool {
	trimmed := bytes.TrimSpace(raw)
	return len(trimmed) >= 2 && trimmed[0] == '{' && trimmed[len(trimmed)-1] == '}'
}

func requireExactObjectFields(
	raw []byte,
	objectName string,
	required []string,
) (map[string]json.RawMessage, error) {
	if !isJSONObject(raw) {
		return nil, fmt.Errorf("%s must be an object", objectName)
	}
	var fields map[string]json.RawMessage
	decoder := json.NewDecoder(bytes.NewReader(raw))
	if err := decoder.Decode(&fields); err != nil {
		return nil, fmt.Errorf("%s is invalid: %v", objectName, err)
	}
	if err := requireJSONEOF(decoder); err != nil {
		return nil, fmt.Errorf("%s has trailing data", objectName)
	}
	allowed := make(map[string]struct{}, len(required))
	for _, name := range required {
		allowed[name] = struct{}{}
	}
	for name := range fields {
		if _, exists := allowed[name]; !exists {
			return nil, fmt.Errorf("%s contains unknown field %q", objectName, name)
		}
	}
	for _, name := range required {
		if _, exists := fields[name]; !exists {
			return nil, fmt.Errorf("%s is missing field %q", objectName, name)
		}
	}
	return fields, nil
}

type jsonShapeState struct {
	tokens int
}

func validateJSONShape(raw []byte) error {
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	state := jsonShapeState{}
	if err := consumeJSONValue(decoder, 1, &state); err != nil {
		return err
	}
	return requireJSONEOF(decoder)
}

func consumeJSONValue(decoder *json.Decoder, depth int, state *jsonShapeState) error {
	if depth > maxJSONDepth {
		return fmt.Errorf("JSON depth exceeds %d", maxJSONDepth)
	}
	token, err := decoder.Token()
	if err != nil {
		return fmt.Errorf("invalid JSON: %v", err)
	}
	state.tokens++
	if state.tokens > maxJSONTokens {
		return fmt.Errorf("JSON token count exceeds %d", maxJSONTokens)
	}
	delim, isDelimiter := token.(json.Delim)
	if !isDelimiter {
		return nil
	}
	switch delim {
	case '{':
		keys := make(map[string]struct{})
		for decoder.More() {
			keyToken, err := decoder.Token()
			if err != nil {
				return fmt.Errorf("invalid object key: %v", err)
			}
			state.tokens++
			key, ok := keyToken.(string)
			if !ok {
				return errors.New("object key is not a string")
			}
			if utf8.RuneCountInString(key) > maxJSONKeyRunes {
				return fmt.Errorf("JSON key exceeds %d runes", maxJSONKeyRunes)
			}
			if _, exists := keys[key]; exists {
				return fmt.Errorf("duplicate JSON key %q", key)
			}
			keys[key] = struct{}{}
			if err := consumeJSONValue(decoder, depth+1, state); err != nil {
				return err
			}
		}
		if _, err := decoder.Token(); err != nil {
			return fmt.Errorf("invalid object ending: %v", err)
		}
	case '[':
		for decoder.More() {
			if err := consumeJSONValue(decoder, depth+1, state); err != nil {
				return err
			}
		}
		if _, err := decoder.Token(); err != nil {
			return fmt.Errorf("invalid array ending: %v", err)
		}
	default:
		return fmt.Errorf("unexpected JSON delimiter %q", delim)
	}
	return nil
}

func requireJSONEOF(decoder *json.Decoder) error {
	if _, err := decoder.Token(); !errors.Is(err, io.EOF) {
		if err == nil {
			return errors.New("multiple JSON values")
		}
		return err
	}
	return nil
}
