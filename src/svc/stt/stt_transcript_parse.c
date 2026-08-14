/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file stt_transcript_parse.c
/// @brief Parser for the streaming server's transcript messages
///

// === Headers files inclusions ==================================================================================== //

#include "stt_transcript_parse.h"

#include <errno.h>
#include <string.h>

#include "log.h"
#include "stt_json.h"

// === Macros definitions ========================================================================================== //

#define STT_TRANSCRIPT_JSON_KEY_MAX (32U)
#define STT_TRANSCRIPT_MS_PER_SEC   (1000.0)
#define STT_TRANSCRIPT_ROUND_HALF   (0.5)

#define STT_FIELD_SEQ       (1U << 0U)
#define STT_FIELD_FINAL     (1U << 1U)
#define STT_FIELD_TYPE      (1U << 2U)
#define STT_FIELD_START     (1U << 3U)
#define STT_FIELD_END       (1U << 4U)
#define STT_FIELD_TEXT      (1U << 5U)
#define STT_REQUIRED_FIELDS (STT_FIELD_SEQ | STT_FIELD_START | STT_FIELD_END | STT_FIELD_TEXT)

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

/**
 * @brief Parse one STT NDJSON line into a subtitle text event payload.
 * @param line JSON line.
 * @param event Parsed payload destination.
 * @return 0 on success, or a negative errno-style value on failure.
 */
int stt_transcript_parse_line(char const* const line, subtitle_text_evt_t* const event)
{
    char const* cursor = line;
    char key[STT_TRANSCRIPT_JSON_KEY_MAX];
    char type[16];
    double start_sec = 0.0;
    double end_sec = 0.0;
    uint32_t seen = 0U;
    uint32_t finality = 0U;
    uint8_t final_value = 0U;
    uint8_t type_value = 0U;
    uint8_t type_marks_finality = 0U;
    uint8_t key_truncated;
    uint8_t text_truncated;
    uint8_t object_done = 0U;

    if ((line == NULL) || (event == NULL))
    {
        return -EINVAL;
    }

    memset(event, 0, sizeof(*event));
    type[0] = '\0';

    stt_json_skip_whitespace(&cursor);
    if (*cursor != '{')
    {
        return -EINVAL;
    }
    cursor++;

    while (object_done == 0U)
    {
        uint32_t field = 0U;
        int status;

        stt_json_skip_whitespace(&cursor);
        if (*cursor == '}')
        {
            cursor++;
            break;
        }

        key_truncated = 0U;
        status = stt_json_parse_string(&cursor, key, sizeof(key), 0U, &key_truncated);
        if (status != 0)
        {
            return status;
        }

        stt_json_skip_whitespace(&cursor);
        if (*cursor != ':')
        {
            return -EINVAL;
        }
        cursor++;
        stt_json_skip_whitespace(&cursor);

        if (key_truncated == 0U)
        {
            if (strcmp(key, "seq") == 0)
            {
                field = STT_FIELD_SEQ;
            }
            else if (strcmp(key, "is_final") == 0)
            {
                field = STT_FIELD_FINAL;
            }
            else if (strcmp(key, "type") == 0)
            {
                field = STT_FIELD_TYPE;
            }
            else if (strcmp(key, "start_sec") == 0)
            {
                field = STT_FIELD_START;
            }
            else if (strcmp(key, "end_sec") == 0)
            {
                field = STT_FIELD_END;
            }
            else if (strcmp(key, "text") == 0)
            {
                field = STT_FIELD_TEXT;
            }
        }

        if ((field != 0U) && ((seen & field) != 0U))
        {
            return -EINVAL;
        }

        switch (field)
        {
        case STT_FIELD_SEQ:
            status = stt_json_parse_u32(&cursor, &event->seq);
            break;

        case STT_FIELD_FINAL:
            status = stt_json_parse_bool(&cursor, &final_value);
            break;

        case STT_FIELD_TYPE:
            status = stt_json_parse_string(&cursor, type, sizeof(type), 1U, NULL);
            if (status == 0)
            {
                if (strcmp(type, "final") == 0)
                {
                    type_value = 1U;
                    type_marks_finality = 1U;
                }
                else if (strcmp(type, "partial") == 0)
                {
                    type_value = 0U;
                    type_marks_finality = 1U;
                }
                else if (strcmp(type, "transcript") == 0)
                {
                    // Streaming-server message discriminator, not a finality
                    // marker: `is_final` alone decides in that dialect.
                    type_marks_finality = 0U;
                }
                else
                {
                    status = -EINVAL;
                }
            }
            break;

        case STT_FIELD_START:
            status = stt_json_parse_double(&cursor, &start_sec);
            break;

        case STT_FIELD_END:
            status = stt_json_parse_double(&cursor, &end_sec);
            break;

        case STT_FIELD_TEXT:
            text_truncated = 0U;
            status =
                stt_json_parse_string(&cursor, event->text, sizeof(event->text), 1U, &text_truncated);
            if ((status == 0) && (text_truncated != 0U))
            {
                LOG_WARNING("stt-parse: truncated JSON text field");
            }
            break;

        default:
            status = stt_json_skip_value(&cursor);
            break;
        }

        if (status != 0)
        {
            return status;
        }
        seen |= field;

        stt_json_skip_whitespace(&cursor);
        if (*cursor == ',')
        {
            cursor++;
            stt_json_skip_whitespace(&cursor);
            if (*cursor == '}')
            {
                return -EINVAL;
            }
        }
        else if (*cursor == '}')
        {
            cursor++;
            object_done = 1U;
        }
        else
        {
            return -EINVAL;
        }
    }

    // Only a "final"/"partial" type carries finality; "transcript" does not, so
    // that dialect must supply `is_final`.
    finality = seen & STT_FIELD_FINAL;
    if ((type_marks_finality != 0U) && ((seen & STT_FIELD_TYPE) != 0U))
    {
        finality |= STT_FIELD_TYPE;
    }

    stt_json_skip_whitespace(&cursor);
    if ((*cursor != '\0') || ((seen & STT_REQUIRED_FIELDS) != STT_REQUIRED_FIELDS)
        || (finality == 0U))
    {
        return -EINVAL;
    }

    if (((seen & STT_FIELD_FINAL) != 0U) && ((finality & STT_FIELD_TYPE) != 0U)
        && (final_value != type_value))
    {
        return -EINVAL;
    }

    event->is_final = ((seen & STT_FIELD_FINAL) != 0U) ? final_value : type_value;

    if ((start_sec < 0.0) || (end_sec < start_sec)
        || (start_sec > (((double)UINT32_MAX - STT_TRANSCRIPT_ROUND_HALF) / STT_TRANSCRIPT_MS_PER_SEC))
        || (end_sec > (((double)UINT32_MAX - STT_TRANSCRIPT_ROUND_HALF) / STT_TRANSCRIPT_MS_PER_SEC)))
    {
        return -EINVAL;
    }

    event->start_ms = (uint32_t)((start_sec * STT_TRANSCRIPT_MS_PER_SEC) + STT_TRANSCRIPT_ROUND_HALF);
    event->end_ms = (uint32_t)((end_sec * STT_TRANSCRIPT_MS_PER_SEC) + STT_TRANSCRIPT_ROUND_HALF);
    return 0;
}

// === End of documentation ======================================================================================== //
