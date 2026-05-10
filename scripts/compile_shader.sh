#!/bin/bash

if [ -z "$1" ]; then
    echo "Usage: $0 <path-to-slang-file>"
    exit 1
fi

SLANG_FILE=$1
BASE_PATH="${SLANG_FILE%.*}"

# Define mappings: "EntryPoint:Suffix"
# This allows you to keep your .vert and .frag naming exactly as requested
STAGES=("vertexMain:vert" "fragmentMain:frag")

for map in "${STAGES[@]}"; do
    # Split the mapping string
    ENTRY="${map%%:*}"
    SUFFIX="${map##*:}"
    
    OUT_FILE="${BASE_PATH}.${SUFFIX}.spv"
    
    echo "--------------------------------------------------"
    echo "Building: $ENTRY -> $OUT_FILE"
    
    # 1. Compile
    # We specify the stage explicitly to help the compiler disambiguate
    STAGE_TYPE="vertex"
    if [[ "$SUFFIX" == "frag" ]]; then STAGE_TYPE="fragment"; fi
    
    slangc "$SLANG_FILE" \
        -target spirv \
        -profile sm_6_3 \
        -entry "$ENTRY" \
        -stage "$STAGE_TYPE" \
        -o "$OUT_FILE"

    if [ $? -ne 0 ]; then
        echo "❌ Slang compilation failed for $ENTRY"
        exit 1
    fi

    # 2. Validate
    if command -v spirv-val >/dev/null 2>&1; then
        echo "Validating $OUT_FILE..."
        spirv-val "$OUT_FILE"
        if [ $? -ne 0 ]; then
            echo "❌ SPIR-V Validation failed for $OUT_FILE"
            exit 1
        fi
        echo "✅ Validated."
    else
        echo "⚠️  spirv-val not found, skipping validation."
    fi
done

echo "--------------------------------------------------"
echo "Success: All stages compiled and validated."