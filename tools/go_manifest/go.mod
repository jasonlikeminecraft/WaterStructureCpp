module waterstructurecpp/go_manifest

go 1.25.5

require (
	github.com/TriM-Organization/bedrock-world-operator v1.4.0
	github.com/Yeah114/WaterStructure v0.0.0
	github.com/Yeah114/blocks v0.0.0-20251025181709-54ab0b294dfe
)

// The oracle module is private and has no fetchable v0.0.0 release. Build
// with ../build_go_manifest.ps1 and an explicit -OracleRoot; that script adds
// version-specific replacements in a temporary go.work without modifying the
// Go/Fatalder source tree.

require (
	github.com/Happy2018new/worldupgrader v1.1.0 // indirect
	github.com/TriM-Organization/merry-memory v0.2.0 // indirect
	github.com/Yeah114/bdump v0.0.0-00010101000000-000000000000 // indirect
	github.com/andybalholm/brotli v1.2.0 // indirect
	github.com/bongnv/go-container v0.1.0 // indirect
	github.com/deatil/go-cryptobin v1.1.1005 // indirect
	github.com/df-mc/goleveldb v1.1.9 // indirect
	github.com/dsnet/compress v0.0.2-0.20210315054119-f66993602bf5 // indirect
	github.com/go-gl/mathgl v1.2.0 // indirect
	github.com/golang/snappy v1.0.0 // indirect
	github.com/google/uuid v1.6.0 // indirect
	github.com/klauspost/compress v1.18.0 // indirect
	github.com/klauspost/pgzip v1.2.5 // indirect
	github.com/mholt/archiver/v3 v3.5.1 // indirect
	github.com/mitchellh/mapstructure v1.5.0 // indirect
	github.com/nwaples/rardecode v1.1.0 // indirect
	github.com/pierrec/lz4/v4 v4.1.2 // indirect
	github.com/sandertv/gophertunnel v1.48.1 // indirect
	github.com/ulikunitz/xz v0.5.9 // indirect
	github.com/vmihailenco/msgpack/v5 v5.4.1 // indirect
	github.com/vmihailenco/tagparser/v2 v2.0.0 // indirect
	github.com/xi2/xz v0.0.0-20171230120015-48954b6210f8 // indirect
	golang.org/x/crypto v0.39.0 // indirect
	golang.org/x/sys v0.35.0 // indirect
)
