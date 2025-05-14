// Netstick - Copyright (c) 2021 Funkenstein Software Consulting.
// See LICENSE.txt for more details.

#include "tlvc.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

//---------------------------------------------------------------------------
// Encode TLVC data: fill header, payload pointer+length, and compute footer checksum.
void tlvc_encode_data(tlvc_data_t *tlvc_, uint16_t tag_, size_t dataLen_, void *data_) {
    tlvc_->header.tag = tag_;
    tlvc_->header.length = dataLen_;

    tlvc_->data = data_;
    tlvc_->dataLen = dataLen_;

    // Compute checksum over header bytes
    uint16_t checksum = 0;
    auto headerBytes = reinterpret_cast<uint8_t *>(&tlvc_->header);
    for (size_t i = 0; i < sizeof(tlvc_header_t); i++) {
        checksum += headerBytes[i];
    }

    // Compute checksum over payload bytes
    auto payloadBytes = reinterpret_cast<uint8_t *>(data_);
    for (size_t i = 0; i < dataLen_; i++) {
        checksum += payloadBytes[i];
    }

    tlvc_->footer.checksum = checksum;
}

//---------------------------------------------------------------------------
// Decode raw TLVC blob (header + payload + footer) into tlvc_data_t, with length
// and checksum checks. Returns true on success, false otherwise.
bool tlvc_decode_data(tlvc_data_t *tlvc_, void *data_, size_t dataLen_) {
    // Must have at least enough room for header+footer
    if (dataLen_ < sizeof(tlvc_header_t) + sizeof(tlvc_footer_t)) {
        return false;
    }

    // Interpret the beginning as the header
    auto header = reinterpret_cast<tlvc_header_t *>(data_);
    size_t payloadLen = header->length;

    // Check that lengths line up
    if (sizeof(tlvc_header_t) + payloadLen + sizeof(tlvc_footer_t) != dataLen_) {
        return false;
    }

    // Compute checksum over header + payload
    auto rawBytes = reinterpret_cast<uint8_t *>(data_);
    uint16_t checksum = 0;
    size_t checksumRange = sizeof(tlvc_header_t) + payloadLen;
    for (size_t i = 0; i < checksumRange; i++) {
        checksum += rawBytes[i];
    }

    // Locate footer immediately after header+payload
    auto footer = reinterpret_cast<tlvc_footer_t *>(rawBytes + checksumRange);

    // Verify checksum
    if (footer->checksum != checksum) {
        return false;
    }

    // Populate the tlvc_data_t structure
    tlvc_->header = *header;
    tlvc_->footer = *footer;
    tlvc_->data = rawBytes + sizeof(tlvc_header_t);
    tlvc_->dataLen = payloadLen;

    return true;
}
