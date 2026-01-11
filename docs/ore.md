# HN4 Orbital Redundancy Encoding (ORE) - Technical Specification

## 1. Overview

Orbital Redundancy Encoding (ORE) is a lossless compression algorithm designed for the HN4 file system. It replaces traditional LZ-style terminology with a nomenclature and implementation based on "Ballistic Orbits" and "Gravity Wells."

While mathematically equivalent to **LZ-style dictionary redundancy elimination**, ORE restructures the wire format and internal logic to **reduce deterministic pattern signatures commonly used for stream fingerprinting**.

## 2. Core Concepts

| Standard Terminology | ORE Terminology | Definition |
| :--- | :--- | :--- |
| **Input Stream** | **Flux** | The raw data being processed. |
| **Output Stream** | **Stream** | The compressed binary output. |
| **Hash Table** | **Gravity Well** | A look-up table mapping 4-byte sequences (with context distortion) to their last observed absolute position. |
| **Match** | **Orbital Echo** | A sequence of bytes that duplicates previous data. |
| **Offset** | **Orbit Delta** | The backward distance (in bytes) to the previous occurrence. |
| **Window** | **Anchor Frame** | The maximum history distance (64KB) accessible for echoes. |
| **Literal** | **Carrier Span** | Raw bytes that could not be compressed. |

## 3. Compression Logic

### 3.1. Structure Detection
Before compression begins, the data is sampled by a "Structure Detector" (formerly entropy heuristic).
1.  Eight bytes are sampled at equidistant intervals.
2.  If the bytes are identical or exhibit ASCII/alignment patterns, the data is deemed structured.
3.  If the data appears random (stochastic), ORE is skipped to prevent CPU waste and potential data expansion.

### 3.2. Gravity Well (Hashing)
The engine maintains a `Gravity Well` (hash table) to track 4-byte sequences.
To obscure standard signatures:
1.  **Flux Distortion:** The hash of the current 4-byte sequence is XORed with the *previous* byte processed. This creates a context dependency.
2.  **Non-Linear Fold:** The result is mixed using rotate-XOR operations rather than standard multiplicative hashing.

```c
key = sequence ^ (previous_byte << 24);
hash = fold_mix(key) >> shift;
```

### 3.3. Orbital Loop
The engine iterates through the input `Flux`:
1.  **Probe:** Calculate gravity hash for current position.
2.  **Check:** Look up index in Gravity Well.
3.  **Validate:**
    *   If index is 0 or too old (> 64KB `Orbit Delta`), it is a collision/miss.
    *   If content matches, an `Echo` is confirmed.
4.  **Update:** The current position is recorded in the Gravity Well before advancing.
5.  **Emit:**
    *   Any skipped raw bytes are written as a `Carrier Span`.
    *   The match is written as an `Orbital Echo`.

## 4. Wire Format

The ORE bitstream consists of a sequence of commands. There is no file header or magic number inside the compressed stream itself.

### 4.1. Command Byte
Every operation starts with a 1-byte Command.

| Bit 7 (Type) | Bits 0-6 (Length) | Description |
| :---: | :---: | :--- |
| `0` | `Len` | **Carrier Span** (Literal). Followed by `Len` raw bytes. |
| `1` | `Len` | **Orbital Echo** (Match). Followed by 2-byte Orbit Delta. No inline data follows. |

### 4.2. Variable Length Encoding
If the length of a span or echo exceeds 127 (the 7-bit limit):
1.  The Command Byte sets `Len = 127` (all ones).
2.  Subsequent bytes encode the remaining length in blocks of 255.
    *   `255` = Add 255 to length, read next byte.
    *   `< 255` = Add value to length, stop.

**Example 1: Length = 300**
*   Command stores: 127
*   Remaining: 300 - 127 = 173
*   **Encoded:** `[Cmd: 127] [173]`
*   Total: 127 + 173 = 300

**Example 2: Length = 700**
*   Cmd: 127 → Remaining 573
*   `[255]` → Remaining 318
*   `[255]` → Remaining 63
*   `[63]`
*   Total: 127 + 255 + 255 + 63 = 700

### 4.3. Orbit Delta (Offset)
Following an Echo command, a 16-bit Little Endian value defines the `Orbit Delta`.
1.  **Raw Delta:** Distance in bytes: `Current_Pos - Match_Pos`.
2.  **Obfuscation:** The raw delta is XORed with `0xA5A5` (`HN4_ORBIT_MASK`).
    *   `Wire_Delta = Raw_Delta ^ 0xA5A5`

This prevents simple bit-pattern scanning for small offsets (like 1, 4, 8).

## 5. Decompression Logic

The decoder processes the stream sequentially.

### 5.1. Carrier Span
1.  Read length `L`.
2.  Copy `L` bytes from Input to Output.
3.  Advance pointers.

### 5.2. Orbital Echo
1.  Read length `L`.
2.  Add `HN4_ORE_MIN_ECHO` (4) to `L` (implicit bias).
3.  Read 16-bit `Wire_Delta`.
4.  Decode: `Orbit_Delta = Wire_Delta ^ 0xA5A5`.
5.  **Absolute Temporal Addressing:**
    *   Calculate absolute source index: `Read_Idx = Write_Idx - Orbit_Delta`.
    *   Copy data from `Buffer_Base[Read_Idx]` to `Buffer_Base[Write_Idx]`.
6.  **Overlap Handling:**
    *   If `Orbit_Delta < Length`, the source overlaps the destination (RLE).
    *   Copy is performed using **forward temporal reconstruction** to preserve self-referential patterns.

## 6. Safety Constraints

1.  **Bounds Checking:** Every read/write checks remaining buffer capacity.
2.  **DoS Protection:** Expansion is bounded by carrier overhead: **worst-case ≈ 1 + (1/127) + (1/255)** per input byte.
3.  **Underflow:** Echoes cannot reference data before the start of the buffer.
4.  **Zero Gravity:** An Orbit Delta of 0 is illegal and triggers a data corruption error.

## 7. Diagram: ORE Stream Structure

```text
[ Stream ]
    |
    +--- [ Command Byte ]
    |       |
    |       +--- Bit 7: Type (0=Carrier, 1=Echo)
    |       +--- Bit 0-6: Length (0..127)
    |
    +--- [ VarInt Extension ] (Optional, if Len=127)
    |       +--- [ Byte: 255 ]
    |       +--- [ Byte: 255 ]
    |       +--- [ Byte: Remainder ]
    |
    +--- [ Payload ]
            |
            +--- (If Carrier) -> [ Raw Bytes ... ]
            |
            +--- (If Echo)    -> [ Orbit Delta (16-bit) ]
                                     |
                                     +-> XOR 0xA5A5
```
