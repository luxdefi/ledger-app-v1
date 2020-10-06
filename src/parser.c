#include "exception.h"
#include "globals.h"
#include "parser.h"
#include "protocol.h"
#include "to_string.h"
#include "types.h"
#include "network_info.h"

#define REJECT(msg, ...) { PRINTF("Rejecting: " msg "\n", ##__VA_ARGS__); THROW_(EXC_PARSE_ERROR, "Rejected"); }

#define ADD_PROMPT(label_, data_, size_, to_string_) ({ \
        if (meta->prompt.count >= NUM_ELEMENTS(meta->prompt.entries)) THROW_(EXC_MEMORY_ERROR, "Tried to add a prompt to full queue"); \
        sub_rv = PARSE_RV_PROMPT; \
        meta->prompt.labels[meta->prompt.count] = PROMPT(label_); \
        meta->prompt.entries[meta->prompt.count].to_string = to_string_; \
        memcpy(&meta->prompt.entries[meta->prompt.count].data, data_, size_); \
        meta->prompt.count++; \
        meta->prompt.count >= NUM_ELEMENTS(meta->prompt.entries); \
    })

#define CALL_SUBPARSER(subFieldName, subParser) { \
        sub_rv = parse_ ## subParser(&state->subFieldName, meta); \
        if (sub_rv != PARSE_RV_DONE) break; \
    }

#define INIT_SUBPARSER(subFieldName, subParser) \
    init_ ## subParser(&state->subFieldName);

static void check_asset_id(Id32 const *const asset_id, parser_meta_state_t *const meta) {
    check_null(asset_id);
    check_null(meta);
    if (meta->first_asset_id_found) {
        if (memcmp(&meta->first_asset_id, asset_id, sizeof(meta->first_asset_id)) != 0)
            REJECT("All asset IDs must be identical");
    } else {
        bool found_valid_asset_id = false;
        for (int i = 0; i < NETWORK_INFO_SIZE; i++) {
            if (memcmp(network_info[i].avax_asset_id, asset_id, sizeof(asset_id_t)) == 0)
                found_valid_asset_id = true;
        }
        if (!found_valid_asset_id)
            REJECT("Asset ID %.*h is not supported", sizeof(asset_id_t), asset_id);

        memcpy(&meta->first_asset_id, asset_id, sizeof(meta->first_asset_id));
        meta->first_asset_id_found = true;
    }
}

void initFixed(struct FixedState *const state, size_t const len) {
    state->filledTo = 0;
    memset(&state->buffer, 0, len);
}

enum transaction_type_id_t convert_type_id_to_type(uint32_t type_id) {
  switch (type_id) {
      case 0: return TRANSACTION_TYPE_ID_BASE;
      case 3: return TRANSACTION_TYPE_ID_IMPORT;
      case 4: return TRANSACTION_TYPE_ID_EXPORT;
      default: REJECT("Invalid transaction type_id; Must be base, export, or import");
  }
}

enum parse_rv parseFixed(struct FixedState *const state, parser_meta_state_t *const meta, size_t const len) {
    size_t const available = meta->input.length - meta->input.consumed;
    size_t const needed = len - state->filledTo;
    size_t const to_copy = available > needed ? needed : available;
    memcpy(&state->buffer[state->filledTo], &meta->input.src[meta->input.consumed], to_copy);
    state->filledTo += to_copy;
    meta->input.consumed += to_copy;
    return state->filledTo == len ? PARSE_RV_DONE : PARSE_RV_NEED_MORE;
}

#define IMPL_FIXED(name) \
    inline enum parse_rv parse_ ## name (struct name ## _state *const state, parser_meta_state_t *const meta) { \
        return parseFixed((struct FixedState *const)state, meta, sizeof(name));\
    } \
    inline void init_ ## name (struct name ## _state *const state) { \
        return initFixed((struct FixedState *const)state, sizeof(state)); \
    }

#define IMPL_FIXED_BE(name) \
    inline enum parse_rv parse_ ## name (struct name ## _state *const state, parser_meta_state_t *const meta) { \
        enum parse_rv sub_rv = PARSE_RV_INVALID; \
        sub_rv = parseFixed((struct FixedState *const)state, meta, sizeof(name)); \
        if (sub_rv == PARSE_RV_DONE) { \
            state->val = READ_UNALIGNED_BIG_ENDIAN(name, state->buf); \
        } \
        return sub_rv; \
    } \
    inline void init_ ## name (struct name ## _state *const state) { \
        return initFixed((struct FixedState *const)state, sizeof(state)); \
    }

IMPL_FIXED_BE(uint16_t);
IMPL_FIXED_BE(uint32_t);
IMPL_FIXED_BE(uint64_t);
IMPL_FIXED(Id32);
IMPL_FIXED(Address);

void init_SECP256K1TransferOutput(struct SECP256K1TransferOutput_state *const state) {
    state->state = 0;
    state->address_n = 0;
    state->address_i = 0;
    INIT_SUBPARSER(uint64State, uint64_t);
}

static void output_prompt_to_string(char *const out, size_t const out_size, output_prompt_t const *const in) {
    check_null(out);
    check_null(in);
    network_info_t const *const network_info = network_info_from_network_id(in->network_id);
    if (network_info == NULL) REJECT("Can't determine network HRP for addresses");
    char const *const hrp = network_info->hrp;

    size_t ix = nano_avax_to_string(out, out_size, in->amount);

    static char const to[] = " to ";
    if (ix + sizeof(to) > out_size) THROW_(EXC_MEMORY_ERROR, "Can't fit ' to ' into prompt value string");
    memcpy(&out[ix], to, sizeof(to));
    ix += sizeof(to) - 1;

    ix += pkh_to_string(&out[ix], out_size - ix, hrp, strlen(hrp), &in->address.val);
}

enum parse_rv parse_SECP256K1TransferOutput(struct SECP256K1TransferOutput_state *const state, parser_meta_state_t *const meta) {
    enum parse_rv sub_rv = PARSE_RV_INVALID;
    switch (state->state) {
        case 0: {
            // Amount; Type is already handled in init_Output
            CALL_SUBPARSER(uint64State, uint64_t);
            PRINTF("OUTPUT AMOUNT: %.*h\n", sizeof(state->uint64State.buf), state->uint64State.buf); // we don't seem to have longs in printf specfiers.
            if (__builtin_uaddll_overflow(state->uint64State.val, meta->sum_of_outputs, &meta->sum_of_outputs)) THROW_(EXC_MEMORY_ERROR, "Sum of outputs overflowed");
            meta->last_output_amount = state->uint64State.val;
            state->state++;
            INIT_SUBPARSER(uint64State, uint64_t);
        }
        case 1:
            // Locktime
            CALL_SUBPARSER(uint64State, uint64_t);
            PRINTF("LOCK TIME: %.*h\n", sizeof(state->uint64State.buf), state->uint64State.buf); // we don't seem to have longs in printf specfiers.
            state->state++;
            INIT_SUBPARSER(uint32State, uint32_t);
        case 2:
            // Threshold
            CALL_SUBPARSER(uint32State, uint32_t);
            PRINTF("Threshold: %d\n", state->uint32State.val);
            state->state++;
            INIT_SUBPARSER(uint32State, uint32_t);
        case 3: // Address Count
            CALL_SUBPARSER(uint32State, uint32_t);
            state->state++;
            state->address_n = state->uint32State.val;
            if (state->address_n != 1) REJECT("Multi-address outputs are not supported");
            INIT_SUBPARSER(addressState, Address);
        case 4: {
            bool should_break = false;
            while (state->state == 4 && !should_break) {
                CALL_SUBPARSER(addressState, Address);
                state->address_i++;
                PRINTF("Output address %d: %.*h\n", state->address_i, sizeof(state->addressState.buf), state->addressState.buf);

                output_prompt_t output_prompt;
                memset(&output_prompt, 0, sizeof(output_prompt));
                if (!(meta->last_output_amount > 0)) REJECT("Assertion failed: last_output_amount > 0");
                output_prompt.amount = meta->last_output_amount;
                output_prompt.network_id = meta->network_id;
                memcpy(&output_prompt.address, &state->addressState.val, sizeof(output_prompt.address));
                // TODO: We can get rid of this if we add back the P/X- in front of an address
                if (memcmp(state->addressState.buf, global.apdu.u.sign.change_address, sizeof(public_key_hash_t)) == 0) {
                    // skip change address
                } else if(meta->type_id == TRANSACTION_TYPE_ID_EXPORT && meta->swap_output) {
                  should_break = ADD_PROMPT(
                      "X to P chain",
                      &output_prompt, sizeof(output_prompt),
                      output_prompt_to_string
                  );
                } else if(meta->type_id == TRANSACTION_TYPE_ID_IMPORT ) {
                  should_break = ADD_PROMPT(
                      "From P chain",
                      &output_prompt, sizeof(output_prompt),
                      output_prompt_to_string
                  );
                } else {
                  should_break = ADD_PROMPT(
                      "Transfer",
                      &output_prompt, sizeof(output_prompt),
                      output_prompt_to_string
                  );
                }

                if (state->address_i == state->address_n) {
                    state->state++;
                } else {
                    INIT_SUBPARSER(addressState, Address);
                }
            }
            if (should_break) break;
        }
        case 5:
            sub_rv = PARSE_RV_DONE;
            break;
    }
    return sub_rv;
}

void init_Output(struct Output_state *const state) {
    state->state = 0;
    INIT_SUBPARSER(uint32State, uint32_t);
}

enum parse_rv parse_Output(struct Output_state *const state, parser_meta_state_t *const meta) {
    enum parse_rv sub_rv = PARSE_RV_INVALID;
    switch (state->state) {
        case 0:
            CALL_SUBPARSER(uint32State, uint32_t);
            state->type = state->uint32State.val;
            state->state++;
            switch (state->type) {
                default: REJECT("Unrecognized ouput type");
                case 0x00000007:
                    INIT_SUBPARSER(secp256k1TransferOutput, SECP256K1TransferOutput);
            }
        case 1:
            switch (state->type) {
                default: REJECT("Unrecognized ouput type");
                case 0x00000007:
                    PRINTF("SECP256K1TransferOutput\n");
                    CALL_SUBPARSER(secp256k1TransferOutput, SECP256K1TransferOutput);
            }
    }
    return sub_rv;
}

void init_TransferableOutput(struct TransferableOutput_state *const state) {
    state->state = 0;
    INIT_SUBPARSER(id32State, Id32);
}

enum parse_rv parse_TransferableOutput(struct TransferableOutput_state *const state, parser_meta_state_t *const meta) {
    PRINTF("***Parse Transferable Output***\n");
    enum parse_rv sub_rv = PARSE_RV_INVALID;

    switch (state->state) {
        case 0: // asset ID
            CALL_SUBPARSER(id32State, Id32);
            PRINTF("Asset ID: %.*h\n", 32, state->id32State.buf);
            check_asset_id(&state->id32State.val, meta);
            state->state++;
            INIT_SUBPARSER(outputState, Output);
        case 1:
            CALL_SUBPARSER(outputState, Output);
    }
    return sub_rv;
}

void init_SECP256K1TransferInput(struct SECP256K1TransferInput_state *const state) {
    state->state = 0;
    state->address_index_i = 0;
    state->address_index_n = 0;
    INIT_SUBPARSER(uint64State, uint64_t);
}

enum parse_rv parse_SECP256K1TransferInput(struct SECP256K1TransferInput_state *const state, parser_meta_state_t *const meta) {
    enum parse_rv sub_rv = PARSE_RV_INVALID;

    switch (state->state) {
        case 0: // Amount
            CALL_SUBPARSER(uint64State, uint64_t);
            state->state++;
            PRINTF("INPUT AMOUNT: %.*h\n", sizeof(uint64_t), state->uint64State.buf);
            if (__builtin_uaddll_overflow(state->uint64State.val, meta->sum_of_inputs, &meta->sum_of_inputs)) THROW_(EXC_MEMORY_ERROR, "Sum of inputs overflowed");
            INIT_SUBPARSER(uint32State, uint32_t);
        case 1: // Number of address indices
            CALL_SUBPARSER(uint32State, uint32_t);
            state->address_index_n = state->uint32State.val;
            state->state++;
            INIT_SUBPARSER(uint32State, uint32_t);
        case 2: // Address indices
            while (true) {
                CALL_SUBPARSER(uint32State, uint32_t);
                state->address_index_i++;
                PRINTF("Address Index %d: %d\n", state->address_index_i, state->uint32State.val);
                if (state->address_index_i == state->address_index_n) {
                    sub_rv = PARSE_RV_DONE;
                    break;
                }
                INIT_SUBPARSER(addressState, Address);
            }
            break; // Forward the break up.
    }

    return sub_rv;
}


void init_Input(struct Input_state *const state) {
    state->state = 0;
    INIT_SUBPARSER(uint32State, uint32_t);
}

enum parse_rv parse_Input(struct Input_state *const state, parser_meta_state_t *const meta) {
    enum parse_rv sub_rv = PARSE_RV_INVALID;

    switch (state->state) {
        case 0:
            CALL_SUBPARSER(uint32State, uint32_t);
            state->type = state->uint32State.val;
            state->state++;
            switch (state->type) {
                default: REJECT("Unrecognized input type");
                case 0x00000005: // SECP256K1 transfer input
                    INIT_SUBPARSER(secp256k1TransferInput, SECP256K1TransferInput);
            }
        case 1:
            switch (state->type) {
                default: REJECT("Unrecognized input type");
                case 0x00000005: // SECP256K1 transfer input
                    PRINTF("SECP256K1 Input\n");
                    CALL_SUBPARSER(secp256k1TransferInput, SECP256K1TransferInput);
            }
    }

    return sub_rv;
}

void init_TransferableInput(struct TransferableInput_state *const state) {
    state->state = 0;
    INIT_SUBPARSER(id32State, Id32);
}

enum parse_rv parse_TransferableInput(struct TransferableInput_state *const state, parser_meta_state_t *const meta) {
    enum parse_rv sub_rv = PARSE_RV_INVALID;
    switch (state->state) {
        case 0: // tx_id
            CALL_SUBPARSER(id32State, Id32);
            state->state++;
            PRINTF("TX_ID: %.*h\n", 32, state->id32State.buf);
            INIT_SUBPARSER(uint32State, uint32_t);
        case 1: // utxo_index
            CALL_SUBPARSER(uint32State, uint32_t);
            PRINTF("UTXO_INDEX: %u\n", state->uint32State.val);
            state->state++;
            INIT_SUBPARSER(id32State, Id32);
        case 2: // asset_id
            CALL_SUBPARSER(id32State, Id32);
            PRINTF("ASSET ID: %u\n", state->uint32State.val);
            check_asset_id(&state->id32State.val, meta);
            state->state++;
            INIT_SUBPARSER(inputState, Input);
        case 3: // Input
            CALL_SUBPARSER(inputState, Input);
    }
    return sub_rv;
}

#define IMPL_ARRAY(name) \
    void init_ ## name ## s (struct name ## s_state *const state) { \
        state->state = 0; \
        state->i = 0; \
        init_uint32_t(&state->len_state); \
    } \
    enum parse_rv parse_ ## name ## s (struct name ## s_state *const state, parser_meta_state_t *const meta) { \
        enum parse_rv sub_rv = PARSE_RV_INVALID; \
        switch (state->state) { \
            case 0: \
                CALL_SUBPARSER(len_state, uint32_t); \
                state->len = READ_UNALIGNED_BIG_ENDIAN(uint32_t, state->len_state.buf); \
                state->state++; \
                if(state->len == 0) break; \
                init_ ## name(&state->item); \
            case 1: \
                while (true) { \
                    PRINTF(#name " %d\n", state->i + 1); \
                    CALL_SUBPARSER(item, name); \
                    state->i++; \
                    if (state->i == state->len) return PARSE_RV_DONE; \
                    init_ ## name(&state->item); \
                } \
                break; \
        } \
        return sub_rv; \
    }

IMPL_ARRAY(TransferableOutput);
IMPL_ARRAY(TransferableInput);

void init_Memo(struct Memo_state *const state) {
    state->state = 0;
    state->n = 0;
    state->i = 0;
    INIT_SUBPARSER(uint32State, uint32_t);
}

enum parse_rv parse_Memo(struct Memo_state *const state, parser_meta_state_t *const meta) {
    enum parse_rv sub_rv = PARSE_RV_INVALID;

    switch (state->state) {
        case 0:
            CALL_SUBPARSER(uint32State, uint32_t);
            state->n = state->uint32State.val;
            state->state++;
        case 1: {
            size_t available = meta->input.length - meta->input.consumed;
            size_t needed = state->n - state->i;
            size_t to_consume = available > needed ? needed : available;
            state->i += to_consume;
            PRINTF("Memo bytes: %.*h\n", to_consume, &meta->input.src[meta->input.consumed]);
            meta->input.consumed += to_consume;
            sub_rv = state->i == state->n ? PARSE_RV_DONE : PARSE_RV_NEED_MORE;
        }
    }

    return sub_rv;
}

void initTransaction(struct TransactionState *const state) {
    state->state = 0;
    init_uint32_t(&state->uint32State);
    cx_sha256_init(&state->hash_state);
}

void update_transaction_hash(cx_sha256_t *const state, uint8_t const *const src, size_t const length) {
    PRINTF("HASH DATA: %d bytes: %.*h\n", length, length, src);
    cx_hash((cx_hash_t *const)state, 0, src, length, NULL, 0);
}

static void strcpy_prompt(char *const out, size_t const out_size, char const *const in) {
    strncpy(out, in, out_size);
}

static bool prompt_fee(parser_meta_state_t *const meta) {
    uint64_t fee = -1; // if this is unset this should be obviously wrong
    if (__builtin_usubll_overflow(meta->sum_of_inputs, meta->sum_of_outputs, &fee)) THROW_(EXC_MEMORY_ERROR, "Difference of outputs from inputs overflowed");
    if (meta->prompt.count >= NUM_ELEMENTS(meta->prompt.entries)) THROW_(EXC_MEMORY_ERROR, "Tried to add a prompt to full queue");
    meta->prompt.labels[meta->prompt.count] = PROMPT("Fee");
    meta->prompt.entries[meta->prompt.count].to_string = nano_avax_to_string_indirect64;
    memcpy(&meta->prompt.entries[meta->prompt.count].data, &fee, sizeof(fee));
    meta->prompt.count++;
    bool should_break = meta->prompt.count >= NUM_ELEMENTS(meta->prompt.entries);
    return should_break;
}

enum parse_rv parseBaseTransaction(struct TransactionState *const state, parser_meta_state_t *const meta) {
    enum parse_rv sub_rv = PARSE_RV_INVALID;
    switch (state->state) {
        case 2: { // Network ID
            INIT_SUBPARSER(uint32State, uint32_t);
            CALL_SUBPARSER(uint32State, uint32_t);
            state->state++;
            PRINTF("Network ID: %.*h\n", sizeof(state->uint32State.buf), state->uint32State.buf);
            meta->network_id = parse_network_id(state->uint32State.val);
            INIT_SUBPARSER(id32State, Id32);
        }
        case 3: // blockchain ID
            CALL_SUBPARSER(id32State, Id32);
            PRINTF("Blockchain ID: %.*h\n", 32, state->id32State.buf);
            network_info_t const *const network_info = network_info_from_network_id(meta->network_id);
            if (network_info == NULL)
              REJECT("Blockchain ID for given network ID not found");
            if (memcmp(network_info->blockchain_id, &state->id32State.val, sizeof(state->id32State.val)) != 0)
              REJECT("Blockchain ID did not match expected value for network ID");
            state->state++;
            INIT_SUBPARSER(outputsState, TransferableOutputs);
        case 4: // outputs
            CALL_SUBPARSER(outputsState, TransferableOutputs);
            PRINTF("Done with outputs\n");
            state->state++;
            INIT_SUBPARSER(inputsState, TransferableInputs);
        case 5: { // inputs
            CALL_SUBPARSER(inputsState, TransferableInputs);
            PRINTF("Done with inputs\n");
            bool should_break = prompt_fee(meta);
            sub_rv = PARSE_RV_PROMPT;
            state->state++;
            INIT_SUBPARSER(memoState, Memo);
            if (should_break) break;
        }
        case 6: // memo
            CALL_SUBPARSER(memoState, Memo);
            PRINTF("Done with memo;\n");
            state->state++;
        case 7:
            PRINTF("Done\n");
            sub_rv = PARSE_RV_DONE;
    }
    return sub_rv;
}

static bool is_pchain(const uint8_t *buff) {
  const size_t chain_size = 32;
  uint8_t pchain_id[chain_size];
  // pchain id is 32 0x00s
  memset(pchain_id, 0, 32);
  return memcmp(buff, pchain_id, chain_size) == 0;
}

enum parse_rv parseImportTransaction(struct TransactionState *const state, parser_meta_state_t *const meta) {
    enum parse_rv sub_rv = PARSE_RV_INVALID;
      switch (state->state) {
        case 2: { // Network ID
            INIT_SUBPARSER(uint32State, uint32_t);
            CALL_SUBPARSER(uint32State, uint32_t);
            state->state++;
            PRINTF("Network ID: %.*h\n", sizeof(state->uint32State.buf), state->uint32State.buf);
            meta->network_id = parse_network_id(state->uint32State.val);
            INIT_SUBPARSER(id32State, Id32);
        }
        case 3: // blockchain ID
            CALL_SUBPARSER(id32State, Id32);
            PRINTF("Blockchain ID: %.*h\n", 32, state->id32State.buf);
            network_info_t const *const network_info = network_info_from_network_id(meta->network_id);
            if (network_info == NULL)
              REJECT("Blockchain ID for given network ID not found");
            if (memcmp(network_info->blockchain_id, &state->id32State.val, sizeof(state->id32State.val)) != 0)
              REJECT("Blockchain ID did not match expected value for network ID");
            state->state++;
            INIT_SUBPARSER(outputsState, TransferableOutputs);
        case 4: // outputs
            CALL_SUBPARSER(outputsState, TransferableOutputs);
            PRINTF("Done with outputs\n");
            state->state++;
            INIT_SUBPARSER(inputsState, TransferableInputs);
        case 5: { // inputs
            CALL_SUBPARSER(inputsState, TransferableInputs);
            PRINTF("Done with inputs\n");
            state->state++;
            INIT_SUBPARSER(memoState, Memo);
        }
        case 6: // memo
            CALL_SUBPARSER(memoState, Memo);
            PRINTF("Done with memo;\n");
            state->state++;
            INIT_SUBPARSER(id32State, Id32);
        case 7: // ChainID
            CALL_SUBPARSER(id32State, Id32);
            if(! is_pchain(state->id32State.buf)) REJECT("Invalid PChain ID");
            state->state++;
            INIT_SUBPARSER(inputsState, TransferableInputs);
            PRINTF("Done with ChainID;\n");

        case 8: {// PChain
            meta->swap_output = true;
            CALL_SUBPARSER(inputsState, TransferableInputs);
            state->state++;
            // Dont set sub_rv
            prompt_fee(meta);
            PRINTF("Done with PChain Address\n");
            break;
        }
        case 9:
             // This is bc we call the parser recursively, and, at the end, it gets called with
             // nothing to parse...But it exits without unwinding the stack, so if we are here,
             // we need to set this in order to exit properly
            sub_rv = PARSE_RV_DONE;
    }
    return sub_rv;
}

enum parse_rv parseExportTransaction(struct TransactionState *const state, parser_meta_state_t *const meta) {
    enum parse_rv sub_rv = PARSE_RV_INVALID;
    switch (state->state) {
        case 2: { // Network ID
            INIT_SUBPARSER(uint32State, uint32_t);
            CALL_SUBPARSER(uint32State, uint32_t);
            state->state++;
            PRINTF("Network ID: %.*h\n", sizeof(state->uint32State.buf), state->uint32State.buf);
            meta->network_id = parse_network_id(state->uint32State.val);
            INIT_SUBPARSER(id32State, Id32);
        }
        case 3: // blockchain ID
            CALL_SUBPARSER(id32State, Id32);
            PRINTF("Blockchain ID: %.*h\n", 32, state->id32State.buf);
            network_info_t const *const network_info = network_info_from_network_id(meta->network_id);
            if (network_info == NULL)
              REJECT("Blockchain ID for given network ID not found");
            if (memcmp(network_info->blockchain_id, &state->id32State.val, sizeof(state->id32State.val)) != 0)
              REJECT("Blockchain ID did not match expected value for network ID");
            state->state++;
            INIT_SUBPARSER(outputsState, TransferableOutputs);
        case 4: // outputs
            CALL_SUBPARSER(outputsState, TransferableOutputs);
            PRINTF("Done with outputs\n");
            state->state++;
            INIT_SUBPARSER(inputsState, TransferableInputs);
        case 5: { // inputs
            CALL_SUBPARSER(inputsState, TransferableInputs);
            PRINTF("Done with inputs\n");
            state->state++;
            INIT_SUBPARSER(memoState, Memo);
        }
        case 6: // memo
            CALL_SUBPARSER(memoState, Memo);
            PRINTF("Done with memo;\n");
            state->state++;
            INIT_SUBPARSER(id32State, Id32);
        case 7: // ChainID
            CALL_SUBPARSER(id32State, Id32);
            if(!is_pchain(state->id32State.buf)) REJECT("Invalid PChain ID");
            state->state++;
            INIT_SUBPARSER(outputsState, TransferableOutputs);
            PRINTF("Done with ChainID;\n");

        case 8: {// PChain Dst
            meta->swap_output = true;
            CALL_SUBPARSER(outputsState, TransferableOutputs);
            state->state++;
            // Dont set sub_rv
            prompt_fee(meta);
            PRINTF("Done with PChain Address\n");
            break;
        }
        case 9:
             // This is bc we call the parser recursively, and, at the end, it gets called with
             // nothing to parse...But it exits without unwinding the stack, so if we are here,
             // we need to set this in order to exit properly
            sub_rv = PARSE_RV_DONE;
    }
    return sub_rv;
}

static char const transactionLabel[] = "Transaction";
static char const importLabel[] = "Import";
static char const exportLabel[] = "Export";

typedef struct { char const* label; size_t label_size; } label_t;

static label_t type_id_to_label(enum transaction_type_id_t type_id) {
  switch (type_id) {
    case TRANSACTION_TYPE_ID_BASE: return (label_t) { .label = transactionLabel, .label_size = sizeof(transactionLabel) };
    case TRANSACTION_TYPE_ID_IMPORT: return (label_t) { .label = importLabel, .label_size = sizeof(importLabel) };
    case TRANSACTION_TYPE_ID_EXPORT: return (label_t) { .label = exportLabel, .label_size = sizeof(exportLabel) };
  }
}

enum parse_rv parseTransaction(struct TransactionState *const state, parser_meta_state_t *const meta) {
    check_null(state);
    check_null(meta);
    check_null(meta->input.src);

    PRINTF("***Parse Transaction***\n");
    enum parse_rv sub_rv = PARSE_RV_INVALID;
    size_t const start_consumed = meta->input.consumed;
    switch (state->state) {
        case 0: // codec ID
            CALL_SUBPARSER(uint16State, uint16_t);
            PRINTF("Codec ID: %d\n", state->uint16State.val);
            if (state->uint16State.val != 0) REJECT("Only codec ID 0 is supported");
            state->state++;
            INIT_SUBPARSER(uint32State, uint32_t);
        case 1: { // type ID
            CALL_SUBPARSER(uint32State, uint32_t);
            state->type = state->uint32State.val;

            // Rejects invalid tx types
            meta->type_id = convert_type_id_to_type(state->type);
            state->state++;
            PRINTF("Type ID: %.*h\n", sizeof(state->uint32State.buf), state->uint32State.buf);

            label_t label = type_id_to_label(meta->type_id);
            if (ADD_PROMPT("Sign", label.label, label.label_size, strcpy_prompt)) break;
        }
        default:
            switch (meta->type_id) {
              case TRANSACTION_TYPE_ID_BASE:
                sub_rv = parseBaseTransaction(state, meta);
                break;
              case TRANSACTION_TYPE_ID_IMPORT:
                sub_rv = parseImportTransaction(state, meta);
                break;
              case TRANSACTION_TYPE_ID_EXPORT:
                sub_rv = parseExportTransaction(state, meta);
                break;

              default:
                REJECT("Only base, export, and import transactions are supported");
            }
    }
    PRINTF("Consumed %d bytes of input so far\n", meta->input.consumed);
    update_transaction_hash(&state->hash_state, &meta->input.src[start_consumed], meta->input.consumed - start_consumed);
    return sub_rv;
}
