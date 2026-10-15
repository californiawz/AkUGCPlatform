package registry

import (
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strings"
	"testing"
)

func TestUnrealManifestSignatureCompatibility(t *testing.T) {
	artifact := []byte(`{
		"manifest": {
			"releaseId": "79658f67-4210-66cf-b4f0-e4990d2a015b",
			"schemaVersion": 6,
			"projectId": "6012a135-4599-3924-7cd4-bca1bff7d4e3",
			"templateId": "official.tower_defense",
			"capabilities": ["logic", "ruleset", "world.spawn", "rules.wave"],
			"assetDependencies": [
				"official.environment.ground",
				"official.gameplay.base",
				"official.gameplay.enemy_spawn",
				"official.gameplay.goal",
				"official.gameplay.path_node",
				"official.tower.basic",
				"official.unit.basic_enemy"
			],
			"contentHash": "e43f74a6669965385dae20fb1534e01ec888dd4a1513631f67f7a34282740ff6"
		},
		"document": {},
		"program": {},
		"signature": {
			"algorithm": "ed25519",
			"publicKey": "e3f665b6e79de2802fb8dc274de92c3d3dfd73f015d3ad86d052eedec8eadaa4",
			"signature": "85466b2d09179a320e7dfaeb3253b63904f83b6256deed048691b4abea6b05b8cf77f54ded84231e8c076395a1988fbb5075918111bf800e9aa70c5a1a3fc907"
		}
	}`)
	metadata, err := InspectAndVerifyEnvelope(artifact)
	if err != nil {
		t.Fatalf("Unreal-generated signature was rejected: %v", err)
	}
	if metadata.ReleaseID != "79658f67-4210-66cf-b4f0-e4990d2a015b" {
		t.Fatalf("unexpected release ID: %s", metadata.ReleaseID)
	}
	if metadata.ArtifactSize != int64(len(artifact)) {
		t.Fatalf("artifact size mismatch: %d", metadata.ArtifactSize)
	}
}

func TestEnvelopeRejectsTamperedManifestAndDuplicateFields(t *testing.T) {
	artifact, _, _ := signedArtifact(t, "11111111-2222-3333-4444-555555555555")
	tampered := strings.Replace(
		string(artifact),
		`"contentHash":"`+strings.Repeat("a", 64)+`"`,
		`"contentHash":"b`+strings.Repeat("a", 63)+`"`,
		1,
	)
	if _, err := InspectAndVerifyEnvelope([]byte(tampered)); err == nil {
		t.Fatal("tampered manifest unexpectedly passed")
	}

	duplicate := strings.Replace(
		string(artifact),
		`"document":`,
		`"document":{},"document":`,
		1,
	)
	if _, err := InspectAndVerifyEnvelope([]byte(duplicate)); err == nil ||
		!strings.Contains(err.Error(), "duplicate JSON key") {
		t.Fatalf("duplicate field error mismatch: %v", err)
	}

	caseVariant := strings.Replace(
		string(artifact),
		`"manifest":`,
		`"Manifest":`,
		1,
	)
	if _, err := InspectAndVerifyEnvelope([]byte(caseVariant)); err == nil ||
		!strings.Contains(err.Error(), `unknown field "Manifest"`) {
		t.Fatalf("case-variant field error mismatch: %v", err)
	}
}

func TestVerifiedPackVerifierRequiresDeepValidation(t *testing.T) {
	artifact, _, publicKey := signedArtifact(t, "21111111-2222-3333-4444-555555555555")
	withoutDeep := VerifiedPackVerifier{}
	if _, err := withoutDeep.Verify(context.Background(), artifact, publicKey); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("missing deep validator should fail closed: %v", err)
	}

	rejecting := VerifiedPackVerifier{Deep: deepValidatorFunc(func(
		context.Context,
		[]byte,
		string,
	) (PackMetadata, error) {
		return PackMetadata{}, errors.Join(ErrInvalidInput, errors.New("content hash mismatch"))
	})}
	if _, err := rejecting.Verify(context.Background(), artifact, publicKey); !errors.Is(err, ErrInvalidInput) {
		t.Fatalf("deep content rejection was lost: %v", err)
	}
}

type deepValidatorFunc func(context.Context, []byte, string) (PackMetadata, error)

func (function deepValidatorFunc) Validate(
	ctx context.Context,
	artifact []byte,
	key string,
) (PackMetadata, error) {
	return function(ctx, artifact, key)
}

func signedArtifact(t *testing.T, releaseID string) ([]byte, ed25519.PrivateKey, string) {
	t.Helper()
	seed := make([]byte, ed25519.SeedSize)
	for index := range seed {
		seed[index] = byte(index + 1)
	}
	privateKey := ed25519.NewKeyFromSeed(seed)
	publicKey := privateKey.Public().(ed25519.PublicKey)
	manifest := packManifest{
		ReleaseID:         releaseID,
		SchemaVersion:     6,
		ProjectID:         "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
		TemplateID:        "official.tower_defense",
		Capabilities:      []string{"logic", "ruleset"},
		AssetDependencies: []string{"official.environment.ground"},
		ContentHash:       strings.Repeat("a", 64),
	}
	signature := ed25519.Sign(privateKey, []byte(deterministicManifest(manifest)))
	envelope := packEnvelope{
		Manifest: manifest,
		Document: json.RawMessage(`{"manifest":{},"scenes":[]}`),
		Program:  json.RawMessage(`{"instructions":[],"gameStartEntryIndex":-1,"waveStartEntryIndex":-1}`),
		Signature: packSignature{
			Algorithm: "ed25519",
			PublicKey: hex.EncodeToString(publicKey),
			Signature: hex.EncodeToString(signature),
		},
	}
	artifact, err := json.Marshal(envelope)
	if err != nil {
		t.Fatal(err)
	}
	return artifact, privateKey, hex.EncodeToString(publicKey)
}
