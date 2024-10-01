#define _GNU_SOURCE // asprintf
#include <stdio.h>
#include <curl/curl.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>

#include "chat-llm.h"
#include "alloc-inl.h"
#include "hash.h"

// -lcurl -ljson-c -lpcre2-8
// apt install libcurl4-openssl-dev libjson-c-dev libpcre2-dev libpcre2-8-0

#define MAX_OUTPUT_TOKENS 4096
#define MAX_INPUT_TOKENS 16384
#define CONFIDENT_TIMES 3

struct MemoryStruct
{
    char *memory;
    size_t size;
};

static size_t chat_with_llm_helper(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    mem->memory = realloc(mem->memory, mem->size + realsize + 1);
    if (mem->memory == NULL)
    {
        /* out of memory! */
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }

    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

char *chat_with_llm(char *prompt, char *model, int tries, float temperature)
{
    CURL *curl;
    CURLcode res = CURLE_OK;
    char *answer = NULL;
    char *url = NULL;

    url = "https://api.openai.com/v1/chat/completions";
    char *auth_header = "Authorization: Bearer " OPENAI_TOKEN;
    char *content_header = "Content-Type: application/json";
    char *accept_header = "Accept: application/json";
    char *data = NULL;
    if (strcmp(model, "instruct") == 0)
    {
        asprintf(&data, "{\"model\": \"gpt-4o-mini\", \"messages\": [{\"role\": \"system\", \"content\": \"You are a helpful assistant.\"}, {\"role\": \"user\", \"content\": \"%s\"}], \"max_tokens\": %d, \"temperature\": %f}", prompt, MAX_OUTPUT_TOKENS, temperature);
    }
    else
    {
        asprintf(&data, "{\"model\": \"gpt-4o-mini\",\"messages\": %s, \"max_tokens\": %d, \"temperature\": %f}", prompt, MAX_OUTPUT_TOKENS, temperature);
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    do
    {
        struct MemoryStruct chunk;

        chunk.memory = malloc(1); /* will be grown as needed by the realloc above */
        chunk.size = 0;           /* no data at this point */

        curl = curl_easy_init();
        if (curl)
        {
            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, auth_header);
            headers = curl_slist_append(headers, content_header);
            headers = curl_slist_append(headers, accept_header);

            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, chat_with_llm_helper);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

            res = curl_easy_perform(curl);

            if (res == CURLE_OK)
            {
                json_object *jobj = json_tokener_parse(chunk.memory);

                // Check if the "choices" key exists
                if (json_object_object_get_ex(jobj, "choices", NULL))
                {
                    json_object *choices = json_object_object_get(jobj, "choices");
                    json_object *first_choice = json_object_array_get_idx(choices, 0);
                    const char *data;

                    json_object *jobj4 = json_object_object_get(first_choice, "message");
                    json_object *jobj5 = json_object_object_get(jobj4, "content");
                    data = json_object_get_string(jobj5);
                    if (data[0] == '\n')
                        data++;
                    answer = strdup(data);
                }
                else
                {
                    printf("Error response is: %s\n", chunk.memory);
                    sleep(2); // Sleep for a small amount of time to ensure that the service can recover
                }
                json_object_put(jobj);
            }
            else
            {
                printf("Error: %s\n", curl_easy_strerror(res));
            }

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }

        free(chunk.memory);
    } while ((res != CURLE_OK || answer == NULL) && (--tries > 0));

    if (data != NULL)
    {
        free(data);
    }

    curl_global_cleanup();
    return answer;
}

char *construct_prompt_stall(char *protocol_name, char *examples, char *history)
{
    char *template = "In the %s protocol, the communication history between the %s client and the %s server is as follows."
                     "The next proper client request that can affect the server's state are:\\n\\n"
                     "Desired format of real client requests:\\n%sCommunication History:\\n\\\"\\\"\\\"\\n%s\\\"\\\"\\\"";

    char *prompt = NULL;
    asprintf(&prompt, template, protocol_name, protocol_name, protocol_name, examples, history);

    char *final_prompt = NULL;

    asprintf(&final_prompt, "[{\"role\": \"system\", \"content\": \"You are a helpful assistant.\"}, {\"role\": \"user\", \"content\": \"%s\"}]", prompt);

    free(prompt);

    return final_prompt;
}

char *construct_prompt_for_templates(char *protocol_name, char **final_msg)
{
    // Give one example for learning formats
    char *prompt_rtsp_example = "For the RTSP protocol, the DESCRIBE client request template is:\\n"
                                "DESCRIBE: [\\\"DESCRIBE <<VALUE>>\\\\r\\\\n\\\","
                                "\\\"CSeq: <<VALUE>>\\\\r\\\\n\\\","
                                "\\\"User-Agent: <<VALUE>>\\\\r\\\\n\\\","
                                "\\\"Accept: <<VALUE>>\\\\r\\\\n\\\","
                                "\\\"\\\\r\\\\n\\\"]";

    char *prompt_http_example = "For the HTTP protocol, the GET client request template is:\\n"
                                "GET: [\\\"GET <<VALUE>>\\\\r\\\\n\\\"]";

    char *msg = NULL;
    asprintf(&msg, "%s\\n%s\\nFor the %s protocol, all of client request templates are (RESPOND IN THE SAME GENERIC FORMAT AS WAS SHOWN):", prompt_rtsp_example, prompt_http_example, protocol_name);
    *final_msg = msg;
    /** Format of prompt_grammars
    prompt_grammars = [
        {"role": "system", "content": "You are a helpful assistant."},
        {"role": "user", "content": msg}
    ]
     **/
    char *prompt_grammars = NULL;

    asprintf(&prompt_grammars, "[{\"role\": \"system\", \"content\": \"You are a helpful assistant.\"}, {\"role\": \"user\", \"content\": \"%s\"}]", msg);

    return prompt_grammars;
}

char *construct_prompt_for_remaining_templates(char *protocol_name, char *first_question, char *first_answer)
{
    char *second_question = NULL;
    asprintf(&second_question, "For the %s protocol, other templates of client requests are:", protocol_name);

    json_object *answer_str = json_object_new_string(first_answer);
    // printf("The First Question\n%s\n\n", first_question);
    // printf("The First Answer\n%s\n\n", first_answer);
    // printf("The Second Question\n%s\n\n", second_question);
    const char *answer_str_escaped = json_object_to_json_string(answer_str);

    char *prompt = NULL;

    asprintf(&prompt,
             "["
             "{\"role\": \"system\", \"content\": \"You are a helpful assistant.\"},"
             "{\"role\": \"user\", \"content\": \"%s\"},"
             "{\"role\": \"assistant\", \"content\": %s },"
             "{\"role\": \"user\", \"content\": \"%s\"}"
             "]",
             first_question, answer_str_escaped, second_question);

    json_object_put(answer_str);
    free(second_question);

    return prompt;
}

char *extract_stalled_message(char *message, size_t message_len)
{

    int errornumber;
    size_t erroroffset;
    // After a lot of iterations, the model consistently responds with an empty line and then a line of text
    pcre2_code *extracter = pcre2_compile("\r?\n?.*?\r?\n", PCRE2_ZERO_TERMINATED, 0, &errornumber, &erroroffset, NULL);
    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(extracter, NULL);
    int rc = pcre2_match(extracter, message, message_len, 0, 0, match_data, NULL);
    char *res = NULL;
    if (rc >= 0)
    {
        size_t *ovector = pcre2_get_ovector_pointer(match_data);
        res = strdup(message + ovector[1]);
    }

    pcre2_match_data_free(match_data);
    pcre2_code_free(extracter);

    return res;
}

char *format_request_message(char *message)
{

    int message_len = strlen(message);
    int max_len = message_len;
    int res_len = 0;
    char *res = ck_alloc(message_len * sizeof(char));
    for (int i = 0; i < message_len; i++)
    {
        // If an \n is not padded with an \r before, we add it
        if (message[i] == '\n' && (i == 0 || (message[i - 1] != '\r')))
        {
            if (res_len == max_len)
            {
                res = ck_realloc(res, max_len + 10);
                max_len += 10;
            }
            res[res_len++] = '\r';
        }

        if (res_len == max_len)
        {
            res = ck_realloc(res, max_len + 10);
            max_len += 10;
        }
        res[res_len++] = message[i];
    }

    // Add \r\n\r\n to ensure that the packet is accepted
    for (int i = 0; i < 2; i++)
    {
        if (res_len == max_len)
        {
            res = ck_realloc(res, max_len + 10);
            max_len += 10;
        }
        res[res_len++] = '\r';
        if (res_len == max_len)
        {
            res = ck_realloc(res, max_len + 10);
            max_len += 10;
        }
        res[res_len++] = '\n';
    }

    if (res_len == max_len)
    {
        res = ck_realloc(res, max_len + 1);
        max_len++;
    }
    res[res_len++] = '\0';
    free(message);
    return res;
}

char *construct_prompt_for_protocol_message_types(char *protocol_name)
{
    /***
     * Prompt to ask the protocol states as follow:
     * ```
     * In the RTSP protocol, the protocol states are:
     *
     * Desired format:
     * <comma_separated_list_of_states_in_uppercase>
     * ```
     * ***/
    char *prompt = NULL;

    // transfer the prompt into string
    asprintf(&prompt, "In the %s protocol, the message types are: \\n\\nDesired format:\\n<comma_separated_list_of_states_in_uppercase_and_without_whitespaces>", protocol_name);

    return prompt;
}

char *construct_prompt_for_requests_to_states(const char *protocol_name,
                                              const char *protocol_state,
                                              const char *example_requests)
{
    /***
     Prompt to ask the sequence of client requests to reach a protocol state as follows:
        ```
        In the RTSP protocol, if the server just starts, to reach the PLAYING state, the sequence of client requests can be:
        DESCRIBE rtsp://127.0.0.1:8554/aacAudioTest RTSP/1.0
        CSeq: 2
        User-Agent: ./testRTSPClient (LIVE555 Streaming Media v2018.08.28)
        Accept: application/sdp

        SETUP rtsp://127.0.0.1:8554/aacAudioTest/track1 RTSP/1.0
        CSeq: 3
        User-Agent: ./testRTSPClient (LIVE555 Streaming Media v2018.08.28)
        Transport: RTP/AVP;unicast;client_port=38784-38785

        PLAY rtsp://127.0.0.1:8554/aacAudioTest/ RTSP/1.0
        CSeq: 4
        User-Agent: ./testRTSPClient (LIVE555 Streaming Media v2018.08.28)
        Session: 000022B8
        Range: npt=0.000-

        Similarly, in the RTSP protocol, if the server just starts, to reach the RECORD state, the sequence of client requests can be:
     ***/

    // Transfer formats of example_requests
    json_object *example_requests_json = json_object_new_string(example_requests);
    const char *example_requests_json_str = json_object_to_json_string(example_requests_json);

    json_object *protocol_state_json = json_object_new_string(protocol_state);
    const char *protocol_state_json_str = json_object_to_json_string(protocol_state_json);

    char *prompt = NULL;

    int example_request_len = strlen(example_requests_json_str) - 2;
    if (example_request_len > EXAMPLE_SEQUENCE_PROMPT_LENGTH)
    {
        example_request_len = EXAMPLE_SEQUENCE_PROMPT_LENGTH;
    }

    asprintf(&prompt,
             "In the %s protocol, if the server just starts, to reach the INIT state, the sequence of client requests can be:\\n"
             "%.*s\\nSimilarly, in the %s protocol, if the server just starts, to reach the %.*s state, the sequence of client requests can be:\\n",
             protocol_name,
             example_request_len,
             example_requests_json_str + 1,
             protocol_name,
             (int)strlen(protocol_state_json_str) - 2,
             protocol_state_json_str + 1);

    json_object_put(protocol_state_json);
    json_object_put(example_requests_json);

    return prompt;
}

void extract_message_grammars(char *answers, klist_t(gram) * grammar_list)
{

    char *ptr = answers;
    int len = strlen(answers);

    while (ptr < answers + len)
    {
        char *start = strchr(ptr, '[');
        if (start == NULL)
            break;
        char *end = strchr(start, ']');
        if (end == NULL)
            break;
        int count = end - start + 1;
        char *temp = (char *)ck_alloc(count + 1);
        strncpy(temp, start, count);
        temp[count] = '\0';
        ptr = end + 1;

        // conver temp to json object and save it to the list
        json_object *jobj = json_tokener_parse(temp);
        *kl_pushp(gram, grammar_list) = jobj;

        // printf("%s\n", temp);
    }
}

int parse_pattern(pcre2_code *replacer, pcre2_match_data *match_data, const char *str, size_t len, char *pattern)
{
    strcat(pattern, "(?:");
    // offset == 3;
    int rc = pcre2_match(replacer, str, len, 0, 0, match_data, NULL);

    if (rc < 0)
    {
        switch (rc)
        {
        case PCRE2_ERROR_NOMATCH:
            // printf("No match for %s!\n", str);
            break;
        default:
            // printf("Matching error %d\n", rc);
            break;
        }
        pcre2_match_data_free(match_data);
        pcre2_code_free(replacer);
        return 0;
    }
    // printf("RC is %d\n",rc);
    PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
    // for(int i = 1; i<rc;i++){
    //     printf("Start %d, end %d\n",ovector[2*i],ovector[2*i+1]);
    // }

    if (rc == 4)
    { // matched the first option - there is a spec
        strncat(pattern, str + ovector[2], ovector[3] - ovector[2]);
        // offset += ovector[3] - ovector[2];

        strcat(pattern, "(.*)");
        // offset += 3;

        strncat(pattern, str + ovector[6], ovector[7] - ovector[6]);
        // offset += ovector[7] - ovector[6];
    }
    else if (rc == 5)
    {
        // matched the second option - there is no special value
        strncat(pattern, str + ovector[8], ovector[9] - ovector[8]);
        // offset += ovector[9] - ovector[8];
    }
    else
    {
        FATAL("Regex groups were updated but not the handling code.");
    }
    strcat(pattern, ")");
    return 1;
}

// If successful, puts 2 patterns in the patterns array, the first one is the header, the second is the fields
// Else returns an array with the first element being NULL
char *extract_message_pattern(const char *header_str, khash_t(field_table) * field_table, pcre2_code **patterns, int debug_file, const char *debug_file_name)
{
    int errornumber;
    size_t erroroffset;
    char header_pattern[128] = {0};
    char fields_pattern[1024] = {0};
    pcre2_code *replacer = pcre2_compile("(?:(.*)(?:<<(.*)>>)(.*))|(.+)", PCRE2_ZERO_TERMINATED, PCRE2_DOTALL, &errornumber, &erroroffset, NULL);
    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(replacer, NULL);
    char *message_type = NULL;
    // int offset = 0;
    /**
     * Example output
     * patterns[0] = (?:PLAY (.*)\r\n)
     * patterns[1] = (?|(?:CSeq: (.*)\r\n)|(?:User-Agent: (.*)\r\n)|(?:Range: (.*)\r\n)|(?:\r\n))
     */

    {
        // We use the string in such an escaped format for easier debugging as the regex library supports parsing it properly
        // The string contains quotations so they are ignored
        header_str++;

        int message_len = 0;
        while (header_str[message_len] != '\0' 
        && header_str[message_len] != ' ' 
        && header_str[message_len] != '\n' 
        && header_str[message_len] != '\r' 
        && header_str[message_len] != '\\' )
        {
            message_len++;
        }
        message_type = ck_alloc(message_len + 1);
        memcpy(message_type, header_str, message_len);
        message_type[message_len] = '\0';

        size_t len = strlen(header_str) - 1;
        strcat(header_pattern, "^"); // Ensure that it captures the start of the string
        if (!parse_pattern(replacer, match_data, header_str, len, header_pattern))
        {
            patterns[0] = NULL;
            return NULL;
        }
    }

    int first = 1;

    strcat(fields_pattern, "(?|");
    for (khiter_t field_t_iter = kh_begin(field_table); field_t_iter != kh_end(field_table); ++field_t_iter)
    {
        if (!kh_exist(field_table, field_t_iter) || kh_value(field_table, field_t_iter) < (TEMPLATE_CONSISTENCY_COUNT / 2 + (TEMPLATE_CONSISTENCY_COUNT % 2)))
            continue;

        if (!first)
        {
            strcat(fields_pattern, "|");
        }
        else
        {
            first = 0;
        }

        json_object *field_v = json_object_new_string(kh_key(field_table, field_t_iter));
        const char *str = json_object_to_json_string(field_v);
        // We use the string in such an escaped format for easier debugging as the regex library supports parsing it properly
        // The string contains quotations so they are ignored
        str++;
        size_t len = strlen(str) - 1;
        int matched = parse_pattern(replacer, match_data, str, len, fields_pattern);
        json_object_put(field_v);
        if (!matched)
        {
            patterns[0] = NULL;
            return NULL;
        }
    }

    strcat(fields_pattern, ")");

    if (first == 1)
    { // convert from (?|) to (.+) when the group is empty
        fields_pattern[1] = '.';
        fields_pattern[2] = '+';
    }

    pcre2_match_data_free(match_data);
    pcre2_code_free(replacer);
    printf("Header pattern is %s\n", header_pattern);
    printf("Fields pattern is %s\n", fields_pattern);

    if (debug_file != -1 && debug_file_name != NULL)
    {
        ck_write(debug_file, header_pattern, strlen(header_pattern), debug_file_name);
        ck_write(debug_file, "\n", 1, debug_file_name);
        ck_write(debug_file, fields_pattern, strlen(fields_pattern), debug_file_name);
    }

    {
        pcre2_code *p = pcre2_compile(header_pattern, PCRE2_ZERO_TERMINATED, 0, &errornumber, &erroroffset, NULL);
        pcre2_jit_compile(p, PCRE2_JIT_COMPLETE);
        patterns[0] = p;
    }
    {
        pcre2_code *p = pcre2_compile(fields_pattern, PCRE2_ZERO_TERMINATED, 0, &errornumber, &erroroffset, NULL);
        pcre2_jit_compile(p, PCRE2_JIT_COMPLETE);
        patterns[1] = p;
    }
    return message_type;
}

range_list starts_with(char *line, int length, pcre2_code *pattern)
{
    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(pattern, NULL);

    int rc = pcre2_match(pattern, line, length, 0, 0, match_data, NULL); // find the first range

    // printf("starts_with rc is %d\n", rc);
    if (rc < 0)
    {
        switch (rc)
        {
        case PCRE2_ERROR_NOMATCH:
            // printf("No match!\n");
            break;
        default:
            // printf("Matching error %d\n", rc);
            break;
        }
        pcre2_match_data_free(match_data);
        range_list res;
        kv_init(res);
        return res;
    }

    range_list dyn_ranges;
    kv_init(dyn_ranges);
    PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
    for (int i = 1; i < rc; i++)
    {
        if (ovector[2 * i] == -1)
            continue;
        // printf("Group %d %d %d\n",i, ovector[2 * i], ovector[2 * i + 1]);
        range v = {.start = ovector[2 * i], .len = ovector[2 * i + 1] - ovector[2 * i], .mutable = 1};
        kv_push(range, dyn_ranges, v);
        // kv_push(range, dyn_ranges, v);
        //  ranges[0][i - 1] = v;
    }
    range v = {.start = ovector[0], .len = ovector[1] - ovector[0], .mutable = 1};
    kv_push(range, dyn_ranges, v); // add the global range at the end

    pcre2_match_data_free(match_data);
    return dyn_ranges;
}

range_list get_mutable_ranges(char *line, int length, int offset, pcre2_code *pattern)
{
    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(pattern, NULL);

    range_list dyn_ranges;
    kv_init(dyn_ranges);

    for (;;) // catch all the other ranges
    {
        int rc = pcre2_match(pattern, line, length, offset, 0, match_data, NULL);
        if (rc < 0)
        {
            switch (rc)
            {
            case PCRE2_ERROR_NOMATCH:
                // printf("No match!\n");
                break;
            default:
                // printf("Matching error %d\n", rc);
                break;
            }
            pcre2_match_data_free(match_data);
            match_data = NULL;
            break;
        }
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        if (offset != ovector[0])
        {
            range v = {.start = offset, .len = ovector[0] - offset, .mutable = 1};
            kv_push(range, dyn_ranges, v);
        }

        // printf("Matched over %d %d\n", ovector[0], ovector[1]);
        for (int i = 1; i < rc; i++)
        {
            if (ovector[2 * i] == -1)
                continue;
            // printf("Group %d %d %d\n",i, ovector[2 * i], ovector[2 * i + 1]);
            range v = {.start = ovector[2 * i], .len = ovector[2 * i + 1] - ovector[2 * i], .mutable = 1};
            kv_push(range, dyn_ranges, v);
            // ranges[0][i - 1] = v;
        }
        if (offset == ovector[1])
        { // in the case the match is empty, we just move a step forward
            offset++;
        }
        else
        {
            offset = ovector[1];
        }
    }

    if (offset < length) // catch anything past the last matched pattern
    {
        range v = {.start = offset, .len = length - offset, .mutable = 1};
        kv_push(range, dyn_ranges, v);
    }

    if (match_data != NULL)
    {
        pcre2_match_data_free(match_data);
    }
    return dyn_ranges;
}

range_list get_mutable_ranges_binary(char *line, int length, int offset, pcre2_code *pattern)
{
    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(pattern, NULL);

    range_list dyn_ranges;
    kv_init(dyn_ranges);

    for (;;) // catch all the other ranges
    {
        int rc = pcre2_match(pattern, line, length, offset, 0, match_data, NULL);
        if (rc < 0)
        {
            switch (rc)
            {
            case PCRE2_ERROR_NOMATCH:
                // printf("No match!\n");
                break;
            default:
                // printf("Matching error %d\n", rc);
                break;
            }
            pcre2_match_data_free(match_data);
            match_data = NULL;
            break;
        }
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        if (offset != ovector[0])
        {
            range v = {.start = offset/2, .len = (ovector[0] - offset)/2, .mutable = 1};
            kv_push(range, dyn_ranges, v);
        }

        // printf("Matched over %d %d\n", ovector[0], ovector[1]);
        for (int i = 1; i < rc; i++)
        {
            if (ovector[2 * i] == -1)
                continue;
            // printf("Group %d %d %d\n",i, ovector[2 * i], ovector[2 * i + 1]);
            range v = {.start = ovector[2 * i]/2, .len = (ovector[2 * i + 1] - ovector[2 * i])/2, .mutable = 1};
            kv_push(range, dyn_ranges, v);
            // ranges[0][i - 1] = v;
        }
        if (offset == ovector[1])
        { // in the case the match is empty, we just move a step forward
            offset++;
        }
        else
        {
            offset = ovector[1];
        }
    }

    if (offset < length) // catch anything past the last matched pattern
    {
        range v = {.start = offset/2, .len = (length - offset)/2, .mutable = 1};
        kv_push(range, dyn_ranges, v);
    }

    if (match_data != NULL)
    {
        pcre2_match_data_free(match_data);
    }
    return dyn_ranges;
}

char *unescape_string(const char *input)
{
    size_t length = strlen(input);
    char *output = (char *)malloc((length + 1) * sizeof(char));

    if (output == NULL)
    {
        printf("Memory allocation failed.\n");
        return NULL;
    }

    size_t i, j = 0;
    for (i = 0; i < length; i++)
    {
        if (input[i] == '\\')
        {
            i++; // Skip the backslash
            switch (input[i])
            {
            case 'n':
                output[j++] = '\n';
                break;
            case 't':
                output[j++] = '\t';
                break;
            case 'r':
                output[j++] = '\r';
                break;
            case '\\':
                output[j++] = '\\';
                break;
            default:
                output[j++] = input[i];
                break;
            }
        }
        else
        {
            output[j++] = input[i];
        }
    }

    output[j] = '\0'; // Add null-terminator to the output string
    return output;
}

void write_new_seeds(char *enriched_file, char *contents)
{
    FILE *fp = fopen(enriched_file, "w");
    if (fp == NULL)
    {
        printf("Error in opening the file %s\n", enriched_file);
        exit(1);
    }

    // remove the newline and whiltespace in the beginning of the string if any
    while (contents[0] == '\n' || contents[0] == ' ' || contents[0] == '\t' || contents[0] == '\r')
    {
        contents++;
    }

    // Check if last 4 characters of the client_request_answer string are \r\n\r\n
    // If not, add them
    int len = strlen(contents);
    if (contents[len - 1] != '\n' || contents[len - 2] != '\r' || contents[len - 3] != '\n' || contents[len - 4] != '\r')
    {
        fprintf(fp, "%s\r\n\r\n", contents);
    }
    else
    {
        fprintf(fp, "%s", contents);
    }

    fclose(fp);
}

char *format_string(char *state_string)
{
    // remove the newline and whiltespace in the beginning of the string if any
    while (state_string[0] == '\n' || state_string[0] == ' ' || state_string[0] == '\t' || state_string[0] == '\r')
    {
        state_string++;
    }

    int len = strlen(state_string);
    while (state_string[len - 1] == '\n' || state_string[len - 1] == '\r' || state_string[len - 1] == ' ' || state_string[len - 1] == '.')
    {
        state_string[len - 1] = '\0';
        len--;
    }

    return state_string;
}

/***
 * Get the protocol states based on self-consistency check
 * pass the parameters: protocol_name, states_set, states_string
 ***/
void get_protocol_message_types(char *state_prompt, khash_t(strSet) * states_set)
{
    khash_t(strMap) *state_to_times = kh_init(strMap); // map from state to times

    for (int i = 0; i < CONFIDENT_TIMES; i++)
    {
        char *state_answer = chat_with_llm(state_prompt, "instruct", MESSAGE_TYPE_RETRIES, 0.5);
        if (state_answer == NULL)
            continue;
        // printf("## Answer from LLM:\n %s\n", state_answer);

        state_answer = format_string(state_answer);

        char *state_tokens = strtok(state_answer, ",");
        while (state_tokens != NULL)
        {
            char *protocol_state = state_tokens;
            protocol_state = format_string(protocol_state);
            // save the state to the map
            int ret;
            khiter_t k = kh_put(strMap, state_to_times, protocol_state, &ret);
            if (ret == 0)
            {
                kh_value(state_to_times, k)++;
            }
            else
            {
                kh_value(state_to_times, k) = 1;
            }

            state_tokens = strtok(NULL, ",");
        }
    }

    // traverse the map and get the states whose times are larger than 0.5 * CONFIDENT_TIMES
    for (khiter_t k = kh_begin(state_to_times); k != kh_end(state_to_times); ++k)
    {
        if (kh_exist(state_to_times, k))
        {
            if (kh_value(state_to_times, k) >= 0.5 * CONFIDENT_TIMES)
            {
                const char *protocol_state = kh_key(state_to_times, k);
                // add the state to the set
                int ret;
                kh_put(strSet, states_set, protocol_state, &ret);
            }
        }
    }
}

khash_t(strSet) * duplicate_hash(khash_t(strSet) * set)
{
    khash_t(strSet) *new_set = kh_init(strSet);

    for (khiter_t k = kh_begin(set); k != kh_end(set); ++k)
    {
        if (kh_exist(set, k))
        {
            const char *val = kh_key(set, k);
            int ret;
            kh_put(strSet, new_set, val, &ret);
        }
    }

    return new_set;
}

// message_set_list generate_combinations(khash_t(strSet)* sequence, int size)
// {
//     if(size == 0)
//     {
//         message_set_list output;
//         kv_init(output);
//         kv_push(khash_t(strSet)*,output,kh_init(strSet));
//         return output;
//     }
//     else
//     {
//         message_set_list subcombinations = generate_combinations(sequence,size-1);
//         message_set_list newCombinations;
//         kv_init(newCombinations);
//         for(int i = 0; i < kv_size(subcombinations);i++)
//         {
//             khash_t(strSet)* target = kv_A(subcombinations,i);
//             khiter_t sequence_iter;
//             for (sequence_iter = kh_begin(sequence); sequence_iter != kh_end(sequence); sequence_iter++)
//             {
//                 if (!kh_exist(sequence, sequence_iter))
//                     continue;
//                 khiter_t k = kh_get(strSet, target, kh_val(sequence,sequence_iter));
//                 if (kh_exist(target, k))
//                     continue;
//                 khash_t(strSet)* newCombination = duplicate_hash(target);
//                 int absent;
//                 kh_put(strSet,newCombination,kh_val(sequence,sequence_iter))    
//             }
//         }
//         return newCombinations;
//     }
// }
void make_combination(khash_t(strSet)* sequence, char** data , message_set_list* res,khiter_t st, khiter_t end, int index, int size);

message_set_list message_combinations(khash_t(strSet)* sequence, int size)
{
    message_set_list res;
    kv_init(res);
    char* data[size];
    make_combination(sequence,data, &res, kh_begin(sequence), kh_end(sequence), 0, size);
    return res;
}

void make_combination(khash_t(strSet)* sequence, char** data , message_set_list* res,khiter_t st, khiter_t end,
                     int index, int size)
{

    if (index == size)
    {
        khash_t(strSet)* combination = kh_init(strSet);
        int absent;
        for (int j=0; j<size; j++){
            kh_put(strSet,combination, data[j],&absent );
        }
        kv_push(khash_t(strSet)*,*res,combination);
        return;
    }
    for (khiter_t i=st; i != end && end-i+1 >= size-index; i++)
    {
        if(!kh_exist(sequence,i))
            continue;
        data[index] = kh_key(sequence,i);
        make_combination(sequence, data,res, i+1, end, index+1, size);
    }
}



int min(int a, int b) {
    return a < b ? a : b;
}

char *enrich_sequence(char *sequence, khash_t(strSet) * missing_message_types)
{
    const char *prompt_template =
        "The following is one sequence of client requests:\\n"
        "%.*s\\n"
        "Please add the %.*s client requests in the proper locations AND RESPOND WITH A CORRECT MESSAGE SEQUENCE FOR THE PROTOCOL. NO YAPPING! The modified sequence of client requests is:";

    int missing_fields_len = 0;
    int missing_fields_capacity = 100;
    char *missing_fields_seq = ck_alloc(missing_fields_capacity);

    khiter_t k;
    int i = 0;
    for (k = kh_begin(missing_message_types); 
    k != kh_end(missing_message_types) && i < min(MAX_ENRICHMENT_MESSAGE_TYPES, kh_size(missing_message_types)); 
    ++k)
    {
        if (!kh_exist(missing_message_types, k))
            continue;
        ++i; // Increment only after seeing a message type
        const char *message_type = kh_key(missing_message_types, k);
        int needed_len = strlen(message_type) + 2; // add for the ', '

        if (missing_fields_len + needed_len > missing_fields_capacity)
        {
            missing_fields_capacity += 2 * needed_len;
            missing_fields_seq = ck_realloc(missing_fields_seq, missing_fields_capacity);
        }

        memcpy(missing_fields_seq + missing_fields_len, message_type, strlen(message_type));
        memcpy(missing_fields_seq + missing_fields_len + needed_len - 2, ", ", 2);

        missing_fields_len += needed_len;
    }
    missing_fields_len -= 2; // ignore the last ', '

    char *prompt = NULL;

    json_object *sequence_escaped = json_object_new_string(sequence);
    const char *sequence_escaped_str = json_object_to_json_string(sequence_escaped);
    sequence_escaped_str++;

    int sequence_len = strlen(sequence_escaped_str) - 1;
    int allowed_tokens = (MAX_INPUT_TOKENS - strlen(prompt_template) - missing_fields_len);
    if (sequence_len > allowed_tokens)
    {
        sequence_len = allowed_tokens;
    }
    asprintf(&prompt, prompt_template, sequence_len, sequence_escaped_str, missing_fields_len, missing_fields_seq);
    ck_free(missing_fields_seq);
    json_object_put(sequence_escaped);

    char *response = chat_with_llm(prompt, "instruct", ENRICHMENT_RETRIES, 0.5);

    free(prompt);

    return response;
}

char *read_file_as_hex_string(const char *file_path) {
    FILE *file = fopen(file_path, "rb");
    if (file == NULL) {
        printf("An error occurred: Could not open file.\n");
        return NULL;
    }

    // Allocate an initial buffer
    size_t buffer_size = 1;  // Start with 1 for the null terminator
    size_t index = 0;
    char *hex_output = (char *)malloc(buffer_size);
    if (hex_output == NULL) {
        printf("An error occurred: Memory allocation failed.\n");
        fclose(file);
        return NULL;
    }
    hex_output[0] = '\0';  // Null-terminate the empty string

    unsigned char byte;
    while (fread(&byte, 1, 1, file) == 1) {
        // Resize buffer to hold the new hex byte (2 characters) without space
        buffer_size += 2;  // 2 chars for hex (no space)
        hex_output = (char *)realloc(hex_output, buffer_size);
        if (hex_output == NULL) {
            printf("An error occurred: Memory reallocation failed.\n");
            fclose(file);
            return NULL;
        }

        // Append the hex representation to the output string without space
        sprintf(hex_output + index, "%02x", byte);
        index += 2;
    }

    fclose(file);
    return hex_output;

}
char *read_file_as_hex_string_using_space(const char *file_path)
{
    FILE *file = fopen(file_path, "rb");
    if (file == NULL) {
        printf("An error occurred: Could not open file.\n");
        return NULL;
    }

    // Allocate an initial buffer
    size_t buffer_size = 1;  // Start with 1 for the null terminator
    size_t index = 0;
    char *hex_output = (char *)malloc(buffer_size);
    if (hex_output == NULL) {
        printf("An error occurred: Memory allocation failed.\n");
        fclose(file);
        return NULL;
    }
    hex_output[0] = '\0';  // Null-terminate the empty string

    unsigned char byte;
    while (fread(&byte, 1, 1, file) == 1) {
        // Resize buffer to hold the new hex byte (2 characters) and a space
        buffer_size += 3;  // 2 chars for hex + 1 for space
        hex_output = (char *)realloc(hex_output, buffer_size);
        if (hex_output == NULL) {
            printf("An error occurred: Memory reallocation failed.\n");
            fclose(file);
            return NULL;
        }

        // Append the hex representation to the output string with space
        sprintf(hex_output + index, "%02x ", byte);
        index += 3;  // 2 for hex characters + 1 for space
    }

    // Remove the trailing space if the string is not empty
    if (index > 0) {
        hex_output[index - 1] = '\0';  // Replace the last space with null terminator
    }

    fclose(file);
    return hex_output;
}

// Helper function for char **remove_space
char *remove_spaces_from_string(const char *str) {
    // Allocate enough memory for the new string (without spaces)
    char *no_space_str = (char *)malloc(strlen(str) + 1);
    if (no_space_str == NULL) {
        printf("Memory allocation failed.\n");
        return NULL;
    }

    int index = 0;
    for (int i = 0; str[i] != '\0'; i++) {
        if (!isspace((unsigned char)str[i])) { // Check if the character is not a space
            no_space_str[index++] = str[i];
        }
    }
    no_space_str[index] = '\0'; // Null-terminate the new string

    return no_space_str;
}

char **remove_space(char **message_list, int size) 
{
    // Allocate memory for the new list of messages without spaces
    char **space_removed_message_list = (char **)malloc(size * sizeof(char *));
    if (space_removed_message_list == NULL) {
        printf("Memory allocation failed.\n");
        return NULL;
    }

    // Process each message in the list
    for (int i = 0; i < size; i++) {
        space_removed_message_list[i] = remove_spaces_from_string(message_list[i]);
        if (space_removed_message_list[i] == NULL) {
            printf("Failed to remove spaces from message %d.\n", i);
            // Clean up allocated memory in case of failure
            for (int j = 0; j < i; j++) {
                free(space_removed_message_list[j]);
            }
            free(space_removed_message_list);
            return NULL;
        }
    }

    // Free existing message list
    for (int i = 0; i < size; i ++) {
        free(message_list[i]);
    }
    free(message_list);

    return space_removed_message_list;
}

void save_byte_sequence_to_file(const char *byte_sequence, const char *file_name) {
    // 공백 없는 16진수 문자열의 길이 계산
    size_t length = strlen(byte_sequence);

    // 16진수 문자열의 길이는 짝수여야 함
    if (length % 2 != 0) {
        fprintf(stderr, "Invalid byte sequence length.\n");
        return;
    }

    // 저장할 바이트 수 계산 (두 자리 16진수 = 1바이트)
    size_t num_bytes = length / 2;

    // 바이트 배열을 할당
    unsigned char *byte_values = (unsigned char*)malloc(num_bytes);
    if (byte_values == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }

    // 16진수 문자열을 실제 바이트 값으로 변환
    char byte_str[3];  // 두 자리의 16진수 값을 저장할 공간 (null terminator 포함)
    byte_str[2] = '\0';  // null-terminate the string

    size_t byte_index = 0;
    for (size_t i = 0; i < length; i += 2) {
        // 두 개의 16진수 문자를 추출
        byte_str[0] = byte_sequence[i];
        byte_str[1] = byte_sequence[i + 1];

        // 16진수 문자열을 실제 바이트 값으로 변환
        byte_values[byte_index++] = (unsigned char)strtol(byte_str, NULL, 16);
    }

    // 바이너리 파일로 저장
    FILE *binary_file = fopen(file_name, "wb");
    if (binary_file == NULL) {
        fprintf(stderr, "File open failed: %s\n", file_name);
        free(byte_values);
        return;
    }
    fwrite(byte_values, 1, num_bytes, binary_file);
    fclose(binary_file);

    printf("%s file saved.\n", file_name);

    // 메모리 해제
    free(byte_values);
}


char *chat_with_llm_structured_outputs(char *prompt, char *model, char *response_format, int tries, float temperature)
{
    CURL *curl;
    CURLcode res = CURLE_OK;
    char *answer = NULL;
    char *url = NULL;

    url = "https://api.openai.com/v1/chat/completions";
    char *auth_header = "Authorization: Bearer " OPENAI_TOKEN;
    char *content_header = "Content-Type: application/json";
    char *accept_header = "Accept: application/json";
    char *data = NULL;
    if (strcmp(model, "instruct") == 0)
    {
        asprintf(&data, "{\"model\": \"gpt-4o-mini\", \"messages\": [{\"role\": \"system\", \"content\": \"You are a helpful assistant.\"}, {\"role\": \"user\", \"content\": \"%s\"}], \"max_tokens\": %d, \"temperature\": %f}", prompt, MAX_OUTPUT_TOKENS, temperature);
    }
    else
    {
        asprintf(&data, "{\"model\": \"gpt-4o-mini\",\"messages\": %s, \"max_tokens\": %d, \"response_format\": %s, \"temperature\": %f}", prompt, MAX_OUTPUT_TOKENS, response_format, temperature);
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    do
    {
        struct MemoryStruct chunk;

        chunk.memory = malloc(1); /* will be grown as needed by the realloc above */
        chunk.size = 0;           /* no data at this point */

        curl = curl_easy_init();
        if (curl)
        {
            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, auth_header);
            headers = curl_slist_append(headers, content_header);
            headers = curl_slist_append(headers, accept_header);

            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, chat_with_llm_helper);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

            res = curl_easy_perform(curl);

            if (res == CURLE_OK)
            {
                json_object *jobj = json_tokener_parse(chunk.memory);

                // Check if the "choices" key exists
                if (json_object_object_get_ex(jobj, "choices", NULL))
                {
                    json_object *choices = json_object_object_get(jobj, "choices");
                    json_object *first_choice = json_object_array_get_idx(choices, 0);
                    const char *data;

                    json_object *jobj4 = json_object_object_get(first_choice, "message");
                    json_object *jobj5 = json_object_object_get(jobj4, "content");
                    data = json_object_get_string(jobj5);
                    if (data[0] == '\n')
                        data++;
                    answer = strdup(data);
                }
                else
                {
                    printf("Error response is: %s\n", chunk.memory);
                    sleep(2); // Sleep for a small amount of time to ensure that the service can recover
                }
                json_object_put(jobj);
            }
            else
            {
                printf("Error: %s\n", curl_easy_strerror(res));
            }

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }

        free(chunk.memory);
    } while ((res != CURLE_OK || answer == NULL) && (--tries > 0));

    if (data != NULL)
    {
        free(data);
    }

    curl_global_cleanup();
    return answer;
}

char *construct_prompt_for_getting_splitted_message(char *hex_dump_message_sequence, char *protocol_name)
{
    /***
     * ```
     * WHOLE_HEX_DUMP_MESSAGE_SEQUENCE
     * ```
     * This is DNS protocol hex dump message.
     * Split this hex dump message sequence into individual messages.
     */
    char *prompt =  "```\\n%s\\n```\\n"
                    "This is %s protocol hex dump message sequence.\\n"
                    "Split this hex dump message sequence into individual messages.";
    asprintf(&prompt, prompt, hex_dump_message_sequence, protocol_name);

    char *formatted_prompt = "[{\"role\": \"system\", \"content\": \"You are a helpful assistant\"}, "
                             "{\"role\": \"user\", \"content\": \"%s\"}]";
    asprintf(&formatted_prompt, formatted_prompt, prompt);

    return formatted_prompt;
}

char *construct_response_format_for_getting_splitted_message()
{
    /***
     * { "type": "json_schema",
     *   "json_schema": {
     *      "name": "hex_dump_message_sequence"
     *      "schema": {
     *          "type": "object",
     *          "properties": {
     *              "hex_dump_message": {
     *                  "type": "array",
     *                  "items": {
     *                      "type": "string"
     *                  }
     *              }
     *          },
     *          "required": ["hex_dump_message"],
     *          "additionalProperties": false
     *      },
     *      "strict": true
     *   }
     * }
     */

    char *response_format = "{\"type\": \"json_schema\","
                            "\"json_schema\": {"
                                "\"name\": \"hex_dump_message_sequence\","
                                "\"schema\": {"
                                    "\"type\": \"object\","
                                    "\"properties\": {"
                                        "\"hex_dump_message\": {"
                                            "\"type\": \"array\","
                                            "\"items\": {"
                                                "\"type\": \"string\""
                                            "}" // items
                                        "}" // hex_dump_message
                                    "}," // properties
                                    "\"required\": [\"hex_dump_message\"],"
                                    "\"additionalProperties\": false"
                                "}," // schema
                                "\"strict\": true"
                            "}" // json_schema
                            "}"; // type


    return response_format;
}

char *construct_prompt_for_getting_mutable_fields(char *hex_dump, char *protocol_name)
{
    /***
     * ```
     * HEX_DUMP_MESSAGE
     * ```
     * This is DNS protocol hex dump message.
     * Split the hex dump message into individual fields according to the protocol structure,
     * and check if the message still functions correctly even if the fields are mutated.
     */
    char *prompt =  "```\\n%s\\n```\\n"
                    "This is %s protocol hex dump message.\\n"
                    "Split the hex dump message into individual fields according to the protocol structure, and check if the message still functions correctly even if the fields are mutated.";
    asprintf(&prompt, prompt, hex_dump, protocol_name);

    char *formatted_prompt = "[{\"role\": \"system\", \"content\": \"You are a helpful assistant\"}, "
                             "{\"role\": \"user\", \"content\": \"%s\"}]";
    asprintf(&formatted_prompt, formatted_prompt, prompt);

    return formatted_prompt;
}

char *construct_response_format_for_getting_mutable_fields()
{
    /***
     * { "type": "json_schema",
     *   "json_schema": {
     *      "name": "structured_protocol_hex_dump_message"
     *      "schema": {
     *          "type": "object",
     *          "properties": {
     *              "structured_hex_dump_message": {
     *                  "type": "array",
     *                  "items": {
     *                      "type": "object",
     *                      "properties": {
     *                          "hex_dump": { "type": "string" },
     *                          "mutable": { "type": "integer",
     *                                       "description": "0 is not functions correctly and 1 is functions correctly even if the fields are mutated.",
     *                                       "enum": [0, 1]
     *                          }
     *                      }
     *                  }
     *              }
     *          }
     *      }
     *   }
     * }
     */

    char *response_format = "{\"type\": \"json_schema\","
                            "\"json_schema\": {"
                                "\"name\": \"structured_protocol_hex_dump_message\","
                                "\"schema\": {"
                                    "\"type\": \"object\","
                                    "\"properties\": {"
                                        "\"structured_hex_dump_message\": {"
                                            "\"type\": \"array\","
                                            "\"items\": {"
                                                "\"type\": \"object\","
                                                "\"properties\": {"
                                                    "\"hex_dump\": {\"type\": \"string\"},"
                                                    "\"mutable\": {\"type\": \"integer\", \"description\": \"0 is not functions correctly and 1 is functions correctly even if the fields are mutated.\", \"enum\": [0, 1]}"
                                                "}," // properties
                                                "\"required\": [\"hex_dump\", \"mutable\"],"
                                                "\"additionalProperties\": false"
                                            "}" // items
                                        "}" // structured_hex_dump_message
                                    "}," // properties
                                    "\"required\": [\"structured_hex_dump_message\"],"
                                    "\"additionalProperties\": false"
                                "}," // schema
                            "\"strict\": true"
                            "}" // json_schema
                        "}"; // type

    return response_format;
}

char *construct_prompt_for_binary_protocol_message_types(char *protocol_name)
{
    /***
     * For the RTSP protocol, all of client request methods are DESCRIBE, OPTIONS, 
     * PAUSE, PLAY, SETUP, TEARDOWN, RECORD, SET_PARAMETER, ANNOUNCE, GET_PARAMETER.
     * In the [Protocol name] protocol, all of client request methods are:
     */
    char *prompt =  "For the RTSP protocol, all of client request methods are DESCRIBE, OPTIONS, PAUSE, PLAY, SETUP, TEARDOWN, RECORD, SET_PARAMETER, ANNOUNCE, GET_PARAMETER.\\n"
                    "In the %s protocol, all of client request methods are:";
    asprintf(&prompt, prompt, protocol_name);

    char *formatted_prompt = "[{\"role\": \"system\", \"content\": \"You are a helpful assistant\"}, "
                             "{\"role\": \"user\", \"content\": \"%s\"}]";
    asprintf(&formatted_prompt, formatted_prompt, prompt);

    return formatted_prompt;
}

char *construct_response_format_for_binary_protocol_message_types()
{
    /***
     * {
     *   "type": "json_schema",
     *   "json_schema": {
     *      "name": "client_request_method_list",
     *      "schema": {
     *          "type": "object",
     *          "properties": {
     *              "client_request_method": {
     *                  "type": "array",
     *                  "items": {"type": "string"}
     *              }
     *          }
     *      }
     *   }
     * }
     */

    char *response_format = "{\"type\": \"json_schema\","
                            "\"json_schema\": {"
                                "\"name\": \"client_request_method_list\","
                                "\"schema\": {"
                                "\"type\": \"object\","
                                    "\"properties\": {"
                                        "\"client_request_method\": {"
                                            "\"type\": \"array\","
                                            "\"items\": {\"type\": \"string\"}"
                                        "}" // client_request_method
                                    "}" // properties
                                "}," // schema
                                "\"required\": [\"client_request_method\"],"
                                "\"additionalProperties\": false"
                                "}" // json_schema
                            "\"strict\": true"
                            "}"; // type

    // printf("====================== response format ======================\n%s\n\n", response_format);

    return response_format;
}

char *construct_prompt_for_binary_protocol_enrich_sequence(char *protocol_name, char *byte_sequence, char *type1, char *type2)
{
    /***
     * In the [Protocol Name] protocol, following is one sequence of client requests: 
     * [Client request byte message sequence]
     * Please add the [type1], [type2] client requests in the proper locations,
     * and the modified sequence of client requests is:
     */
    char *prompt =  "In the %s protocol, following is one sequence of client requests:\\n"\
                    "%s\\n" // byte sequence string
                    "Please add the %s, %s client requests in the proper locations, and the modified sequence of client requests is:";
    asprintf(&prompt, prompt, protocol_name, byte_sequence, type1, type2);

    char *formatted_prompt = "[{\"role\": \"system\", \"content\": \"You are a helpful assistant\"}, "
                             "{\"role\": \"user\", \"content\": \"%s\"}]";
    asprintf(&formatted_prompt, formatted_prompt, prompt);

    return formatted_prompt;
}

char *construct_response_format_for_binary_protocol_enrich_sequence()
{
    /***
     * {
     *   "type": "json_schema",
     *   "json_schema": {
     *      "name": "client_request_byte_sequence_string",
     *      "schema": {
     *          "type": "object",
     *          "properties": {
     *              "client_request_byte_sequence_string": { "type": "string" }
     *          }
     *          "required": ["client_request_byte_sequence_string"],
     *          "additionalProperties": false
     *      },
     *      "strict": true
     *   }
     * }
     */

    char *response_format = "{\"type\": \"json_schema\","
                            "\"json_schema\": {"
                            "\"name\": \"client_request_byte_sequence_string\","
                            "\"schema\": {"
                            "\"type\": \"object\","
                            "\"properties\": {"
                            "\"client_request_byte_sequence_string\": { \"type\": \"string\" }"
                            "}" // properties
                            "\"required\": [\"client_request_byte_sequence_string\"],"
                            "\"additionalProperties\": false"
                            "}," // schema
                            "\"strict\": true"
                            "}" // json_schema
                            "}"; // type

    return response_format;
}


char **get_splitted_message_from_llm_response(char *response, int *size)
{
    // JSON 객체를 파싱
    struct json_object *parsed_json;
    struct json_object *hex_dump_message;
    
    parsed_json = json_tokener_parse(response);
    
    if (parsed_json == NULL) {
        printf("Invalid JSON string\n");
        *size = 0;
        return NULL;
    }

    // hex_dump_message 배열 추출
    if (json_object_object_get_ex(parsed_json, "hex_dump_message", &hex_dump_message)) {
        int array_len = json_object_array_length(hex_dump_message);
        *size = array_len;

        // 메모리 할당
        char **result = malloc(array_len * sizeof(char*));
        for (int i = 0; i < array_len; i++) {
            struct json_object *msg = json_object_array_get_idx(hex_dump_message, i);
            const char *message = json_object_get_string(msg);

            // 문자열 복사 및 할당
            result[i] = strdup(message);
        }

        // JSON 객체 해제
        json_object_put(parsed_json);

        return result;
    } else {
        printf("hex_dump_message key not found\n");
        *size = 0;
        json_object_put(parsed_json);
        return NULL;
    }
}

pcre2_code **get_binary_message_pattern_from_llm_response(char *response, int *size)
{
    typedef struct {
        char *hex_dump;
        int is_mutable;
    } StructuredHexDump;

    // Make response to json_object
    json_object *parsed_response = json_tokener_parse(response);
    if (parsed_response == NULL) {
        printf("Failed to parse JSON\n");
        return NULL;
    }

    // Get structured_hex_dump list
    json_object *structured_hex_dump = NULL;
    if (!json_object_object_get_ex(parsed_response, "structured_hex_dump_message", &structured_hex_dump)) {
        printf("Invalid JSON format\n");
        json_object_put(parsed_response); // Free memory
        return NULL;
    }
    // Get length of structured_hex_dump
    int n_items = json_object_array_length(structured_hex_dump);
    int mutable_items = 0;
    StructuredHexDump *struct_list = (StructuredHexDump *)malloc(sizeof(StructuredHexDump) * n_items);

    // Get hex_dump and mutable fields
    for (int i = 0; i < n_items; i++) {
        // Get hex dump object from structured_hex_dump at index i.
        json_object *hex_obj = json_object_array_get_idx(structured_hex_dump, i);

        json_object *hex_dump = NULL;
        json_object *mutable_flag = NULL;

        // Get hex_dump field
        if (!json_object_object_get_ex(hex_obj, "hex_dump", &hex_dump)) {
            printf("Missing hex_dump field\n");
            continue;
        }
        // Get mutable field
        if (!json_object_object_get_ex(hex_obj, "mutable", &mutable_flag)) {
            printf("Missing mutable field\n");
            continue;
        }

        int is_mutable = json_object_get_int(mutable_flag);
        if (is_mutable) { mutable_items++; }

        struct_list[i].hex_dump = json_object_get_string(hex_dump);
        struct_list[i].is_mutable = is_mutable;
    }

    pcre2_code **pattern_list = ck_alloc(mutable_items * sizeof(pcre2_code *));

    // Make pattern from struct_list
    for (int it = 0; it < mutable_items; it++) {
        int mutable_flag = 1;
        char pattern[2048] = {0};
        strcat(pattern, "(?:");
        for (int i = 0; i < n_items; i++) {
            if (struct_list[i].is_mutable && mutable_flag) {
                strcat(pattern, "(.*)");
                struct_list[i].is_mutable = 0;
                mutable_flag = 0;
            }
            else {
                strcat(pattern, struct_list[i].hex_dump);
            }
        }
        strcat(pattern, ")");
        // Debug
        printf("Pattern - %d: %s\n", it, pattern);

        // Compile pattern
        int errornumber;
        size_t erroroffset;
        pcre2_code *p = pcre2_compile(pattern, PCRE2_ZERO_TERMINATED, 0, &errornumber, &erroroffset, NULL);
        pcre2_jit_compile(p, PCRE2_JIT_COMPLETE);
        pattern_list[it] = p;
    }

    *size = mutable_items;
    // Free memory
    free(struct_list);
    json_object_put(parsed_response);

    return pattern_list;
}

char *get_hexdump_from_buffer(char *buf, size_t buf_len) {
    if (buf == NULL || buf_len == 0) {
        return NULL;
    }

    // Each byte will be represented by two hex digits
    char *hexdump = malloc(buf_len * 2 + 1); // +1 for null terminator
    if (hexdump == NULL) {
        return NULL; // Memory allocation failed
    }

    for (size_t i = 0; i < buf_len; i++) {
        sprintf(&hexdump[i * 2], "%02x", (unsigned char)buf[i]);
    }
    hexdump[buf_len * 2] = '\0'; // Null-terminate the string

    return hexdump;
}

// // For debugging
// // gcc -g -o chat-llm chat-llm.c chat-llm.h -lcurl -ljson-c -lpcre2-8
// int main(int argc, char **argv)
// {
//     char *protocol_name = argv[1];
//     char *in_dir = argv[2];
//     khash_t(strSet) *states_set = kh_init(strSet);

//     char *state_prompt = construct_prompt_for_protocol_states(protocol_name);

//     // Get protocol states
//     get_protocol_message_types(state_prompt, states_set);

//     // traverse the states_set
//     khiter_t k;
//     for (k = kh_begin(states_set); k != kh_end(states_set); ++k)
//     {
//         if (kh_exist(states_set, k))
//         {
//             const char *protocol_state = kh_key(states_set, k);
//             printf("## State_traverse: %s\n", protocol_state);
//         }
//     }

//     // Get seeds to states and save them to the in_dir
//     get_seeds_to_states(in_dir, states_set, protocol_name);

//     // char *prompt = NULL;
//     // asprintf(&prompt, "user: The colors of flowers:\\nassistant: red and yellow.\\nuser: Other colors are:");
//     // printf("## Prompt to LLM:\n %s\n", prompt);
//     // char *answer = chat_with_llm(prompt, "instruct");
//     // printf("## Answer from LLM:\n %s\n", answer);

//     char *protocol_name = argv[1];
//     khash_t(consistency_table) *const_table = kh_init(consistency_table);
//     klist_t(rang) *protocol_patterns = kl_init(rang);

//     for (int iter = 0; iter < 5; iter++)
//     {

//         char *templates_prompt = construct_prompt_for_templates(protocol_name);
//         char *templates_answer = chat_with_llm(templates_prompt, "turbo");
//         // printf("## Answer from LLM:\n %s\n", templates_answer);
//         char *remaining_prompt = construct_prompt_for_remaining_templates(protocol_name, templates_prompt, templates_answer);
//         // printf("remaining prompt is:\n %s\n", remaining_prompt);
//         char *remaining_templates = chat_with_llm(remaining_prompt, "turbo");
//         // printf("## Remaining templates:\n %s\n", remaining_templates);

//         char *combined_templates = NULL;
//         asprintf(&combined_templates, "%s\n%s", templates_answer, remaining_templates);

//         printf("The final info is\n%s\n", combined_templates);
//         klist_t(gram) *grammar_list = kl_init(gram);
//         extract_message_grammars(combined_templates, grammar_list);

//         kliter_t(gram) * iter;
//         for (iter = kl_begin(grammar_list); iter != kl_end(grammar_list); iter = kl_next(iter))
//         {
//             json_object *jobj = kl_val(iter);

//             json_object *header = json_object_array_get_idx(jobj, 0);

//             int absent;

//             const char *header_str = json_object_get_string(header);

//             khiter_t k = kh_put(consistency_table, const_table, header_str, &absent);
//             if (absent)
//             {
//                 khash_t(field_table) *field_table = kh_init(field_table);
//                 kh_value(const_table, k) = field_table;
//             }

//             for (int i = 1; i < json_object_array_length(jobj); i++)
//             {
//                 const char *v = json_object_get_string(json_object_array_get_idx(jobj, i));
//                 khash_t(field_table) *field_table = kh_value(const_table, k);
//                 khiter_t field_k = kh_put(field_table, field_table, v, &absent);
//                 if (absent)
//                 {
//                     kh_value(field_table, field_k) = 0;
//                 }
//                 kh_value(field_table, field_k)++;
//             }
//         }
//         kl_destroy_gram(grammar_list);
//     }

//     for (khiter_t con_t_iter = kh_begin(const_table); con_t_iter != kh_end(const_table); ++con_t_iter)
//     {
//         if (kh_exist(const_table, con_t_iter))
//         {
//             pcre2_code **patterns = ck_alloc(2 * sizeof(pcre2_code *));

//             khash_t(field_table) *field_table = kh_value(const_table, con_t_iter);
//             const char* header_str = json_object_to_json_string(json_object_new_string(kh_key(const_table, con_t_iter)));

//             extract_message_pattern_k(header_str,field_table, patterns);
//             *kl_pushp(rang, protocol_patterns) = patterns;
//         }
//     }

//     char *demo_lines[] = {

//         "DESCRIBE 123\r\n"
//         "CSeq: 1212313\r\n"
//         "User-Agent: 1212313\r\n"
//         "Accept: 1212313\r\n"
//         "\r\n",

//         "DESCRIBE 123\r\n"
//         "DESCRIBE 123\r\n"
//         "User-Agent: 1212313\r\n"
//         "CSeq: 1212313\r\n"
//         "Accept: 1212313\r\n"
//         "\r\n",

//         "DESCRIBE 123\r\n"
//         "1231321321321"
//         "User-Agent: 1212313\r\n"
//         "CSeq: 1212313\r\n"
//         "Accept: 1212313\r\n"
//         "\r\n"
//         "1231321321321",

//         "DESCRIBE 123\r\n"
//         "1231321321321"
//         "User-Agent: 1212313\r\n"
//         "CSeq: 1212313\r1231321321321\n"
//         "Accept: 1212313\r\n"
//         "\r\n"
//         "1231321321321",

//         "PLAY 123\r\n"
//         "CSeq: 1212313\r\n"
//         "DESCRIBE 123\r\n"
//         "User-Agent: 1212313\r\n"
//         "Session: 1212313\r\n"
//         "Range: 1212313\r\n"
//         "\r\n",

//     };

// char* answers = "For the RTSP protocol, the DESCRIBE client request template is:"
//     "{\"DESCRIBE\":\"string\\r\\n\",\"CSeq:\":\"integer\\r\\n\",\"User-Agent:\":\"string\\r\\n\",\"Accept:\":\"string\\r\\n\\r\\n\"}."
//     "For the RTSP protocol, the DESCRIBE client request template is:{\"DESCRIBE\":\"string\\r\\n\",\"CSeq:\":\"integer\\r\\n\",\"User-Agent:\":\"string\\r\\n\",\"Accept:\":\"string\\r\\n\\r\\n\"}";

// for (int demo = 0; demo < sizeof(demo_lines) / sizeof(char *); demo++)
// {
//     printf("\nTrying to match \n%s\n\n", demo_lines[demo]);
//     int max_rc = -1;
//     kliter_t(rang) * iter_rang;
//     range_list max_ranges;
//     int i = 0;
//     for (iter_rang = kl_begin(protocol_patterns); iter_rang != kl_end(protocol_patterns); iter_rang = kl_next(iter_rang),i++)
//     {
//         // printf("Compare! \n");

//         pcre2_code **patterns = kl_val(iter_rang);
//         pcre2_code *header_pattern = patterns[0];
//         pcre2_code *fields_pattern = patterns[1];

//         range_list header_ranges = starts_with(demo_lines[demo], strlen(demo_lines[demo]), header_pattern);
//         kv_init(header_ranges);

//         if (kv_size(header_ranges) == 0)
//         {
//             printf("Demo %d Did not match pattern %d\n", demo, i);
//             continue;
//         }
//         else
//         {
//             printf("Demo %d Did matched pattern %d\n", demo, i);
//             range header_match = kv_pop(header_ranges);
//             char *offsetted_line = demo_lines[demo];
//             size_t offsetted_len = strlen(demo_lines[demo]);
//             range_list field_ranges = get_mutable_ranges(offsetted_line,offsetted_len, header_match.len,fields_pattern);

//             for(int i = 0; i < kv_size(field_ranges);i++){
//                 kv_push(range, header_ranges, kv_A(field_ranges,i));
//             }
//             kv_destroy(field_ranges);

//             max_ranges = header_ranges;

//             break;
//         }
//     }

//     if (max_rc != -1)
//     {
//         printf("Matched! \n");
//         for (int i = 0; i < max_rc; i++)
//         {
//             printf("start=%d len=%d mutable=%d\n", kv_A(max_ranges,i).start,kv_A(max_ranges,i).len, kv_A(max_ranges,i).mutable);
//             printf("content=%s\n", json_object_to_json_string(json_object_new_string_len(demo_lines[demo] + kv_A(max_ranges,i).start, kv_A(max_ranges,i).len)));
//         }
//     }
//     else
//     {
//         printf("No matches\n");
//     }
// }

// Traverse the list

//     return 0;
// }
