/* SPDX-License-Identifier: MIT */
/* Shared, side-effect-free Standard-DV eligibility. The kernel and player
 * copies must remain identical (checked by the source tests). This only
 * parses public display capabilities; it does not alter EDID or movie data.
 */
#ifndef DVBRIDGE_EDID_H
#define DVBRIDGE_EDID_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#endif

enum dvbridge_edid_status {
	DV_EDID_ABSENT, DV_EDID_INVALID, DV_EDID_NO_DV, DV_EDID_DUPLICATE,
	DV_EDID_VERSION, DV_EDID_LENGTH, DV_EDID_LLDV_ONLY, DV_EDID_STANDARD
};

struct dvbridge_sink_caps {
	enum dvbridge_edid_status status;
	unsigned int version, offset, length;
	bool legacy_vsif, supports_2160p60;
};

static inline const char *dvbridge_edid_reason(enum dvbridge_edid_status status)
{
	switch (status) {
	case DV_EDID_ABSENT: return "edid-unavailable";
	case DV_EDID_INVALID: return "edid-invalid";
	case DV_EDID_NO_DV: return "no-dv-block";
	case DV_EDID_DUPLICATE: return "multiple-dv-blocks";
	case DV_EDID_VERSION: return "unknown-dv-version";
	case DV_EDID_LENGTH: return "invalid-dv-block-length";
	case DV_EDID_LLDV_ONLY: return "no-standard-dv-interface";
	case DV_EDID_STANDARD: return "standard-dv";
	}
	return "unknown";
}

static inline struct dvbridge_sink_caps
dvbridge_parse_edid(const unsigned char *edid, size_t size)
{
	static const unsigned char magic[8] = {0, 255, 255, 255, 255, 255, 255, 0};
	struct dvbridge_sink_caps result = {DV_EDID_ABSENT, 0, 0, 0, false, false};
	unsigned int block, i, found = 0;

	if (!edid || !size)
		return result;
	result.status = DV_EDID_INVALID;
	if (size < 128 || size != ((size_t)edid[126] + 1) * 128)
		return result;
	for (i = 0; i < 8; i++)
		if (edid[i] != magic[i])
			return result;
	/* Check every block before interpreting any capability. */
	for (block = 0; block < size / 128; block++) {
		unsigned char sum = 0;
		for (i = 0; i < 128; i++)
			sum += edid[block * 128 + i];
		if (sum)
			return result;
	}
	result.status = DV_EDID_NO_DV;
	for (block = 1; block < size / 128; block++) {
		const unsigned char *b = edid + block * 128;
		unsigned int end = b[2], pos;

		if (b[0] != 2 || b[1] < 3 || !end)
			continue;
		if (end < 4 || end > 127) {
			result.status = DV_EDID_INVALID;
			return result;
		}
		for (pos = 4; pos < end;) {
			unsigned int len = b[pos] & 31;
			const unsigned char *data = b + pos + 1;

			if (pos + len + 1 > end) {
				result.status = DV_EDID_INVALID;
				return result;
			}
			if ((b[pos] >> 5) == 7 && len >= 4 && data[0] == 1 &&
			    data[1] == 0x46 && data[2] == 0xd0 && data[3] == 0) {
				if (++found != 1) {
					result.status = DV_EDID_DUPLICATE;
					return result;
				}
				result.offset = block * 128 + pos + 1;
				result.length = len;
				result.status = DV_EDID_LENGTH;
				if (len < 5)
					return result;
				result.version = data[4] >> 5;
				switch (result.version) {
				case 0:
					if (len != 21)
						return result;
					result.legacy_vsif = true;
					result.supports_2160p60 = (data[4] & 2) != 0;
					break;
				case 1:
					if (len != 11 && len != 14)
						return result;
					/* Long V1 and short V1 without LL use HDMI-1.4b
					 * signaling, not the modern Dolby-OUI packet. */
					result.legacy_vsif = len == 14 || !(data[7] & 1);
					result.supports_2160p60 = (data[4] & 2) != 0;
					break;
				case 2:
					if (len < 11)
						return result;
					if ((data[6] & 3) < 2) {
						result.status = DV_EDID_LLDV_ONLY;
						return result;
					}
					result.supports_2160p60 = true;
					break;
				default:
					result.status = DV_EDID_VERSION;
					return result;
				}
				result.status = DV_EDID_STANDARD;
			}
			pos += len + 1;
		}
	}
	return result;
}
#endif
