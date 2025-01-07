#ifndef HEADER_fd_src_flamenco_runtime_fd_runtime_h
#define HEADER_fd_src_flamenco_runtime_fd_runtime_h

#include "../fd_flamenco_base.h"
#include "fd_runtime_err.h"
#include "fd_runtime_init.h"
#include "fd_rocksdb.h"
#include "fd_acc_mgr.h"
#include "../features/fd_features.h"
#include "fd_rent_lists.h"
#include "../../ballet/poh/fd_poh.h"
#include "../leaders/fd_leaders.h"
#include "context/fd_exec_epoch_ctx.h"
#include "context/fd_exec_slot_ctx.h"
#include "context/fd_capture_ctx.h"
#include "context/fd_exec_txn_ctx.h"
#include "info/fd_block_info.h"
#include "info/fd_instr_info.h"
#include "../gossip/fd_gossip.h"
#include "../repair/fd_repair.h"
#include "../../ballet/pack/fd_microblock.h"

/* Various constant values used by the runtime. */

#define MICRO_LAMPORTS_PER_LAMPORT (1000000UL)

#define DEFAULT_HASHES_PER_TICK  (12500)
#define UPDATED_HASHES_PER_TICK2 (17500)
#define UPDATED_HASHES_PER_TICK3 (27500)
#define UPDATED_HASHES_PER_TICK4 (47500)
#define UPDATED_HASHES_PER_TICK5 (57500)
#define UPDATED_HASHES_PER_TICK6 (62500)

#define FD_RUNTIME_TRACE_NONE   (0)
#define FD_RUNTIME_TRACE_SAVE   (1)
#define FD_RUNTIME_TRACE_REPLAY (2)

#define FD_RUNTIME_NUM_ROOT_BLOCKS (32UL)

#define FD_FEATURE_ACTIVE(_slot_ctx, _feature_name)  (_slot_ctx->slot_bank.slot >= _slot_ctx->epoch_ctx->features. _feature_name)
#define FD_FEATURE_JUST_ACTIVATED(_slot_ctx, _feature_name)  (_slot_ctx->slot_bank.slot == _slot_ctx->epoch_ctx->features. _feature_name)

#define FD_BLOCKHASH_QUEUE_MAX_ENTRIES    (300UL)
#define FD_RECENT_BLOCKHASHES_MAX_ENTRIES (150UL)

#define FD_RENT_EXEMPT_RENT_EPOCH (ULONG_MAX)

#define SECONDS_PER_YEAR ((double)(365.242199 * 24.0 * 60.0 * 60.0))

/* TODO: increase this to default once we have enough memory to support a 95G status cache. */
#define MAX_CACHE_TXNS_PER_SLOT (FD_TXNCACHE_DEFAULT_MAX_TRANSACTIONS_PER_SLOT / 8)

/* This is the reasonably tight upper bound for the number of writable 
   accounts in a slot. This is because a block has a limit of 48 million
   compute units. Each writable account lock costs 300 CUs. That means there
   can be up to 48M/300 writable accounts in a block. */
#define FD_WRITABLE_ACCS_IN_SLOT (160000UL)

struct fd_execute_txn_task_info {
  fd_spad_t * * spads;
  fd_exec_txn_ctx_t * txn_ctx;
  fd_txn_p_t * txn;
  int exec_res;
};
typedef struct fd_execute_txn_task_info fd_execute_txn_task_info_t;

struct fd_raw_block_txn_iter {
  fd_block_entry_batch_t const * curr_batch;
  uchar const * orig_data;
  ulong remaining_batches;
  ulong remaining_microblocks;
  ulong remaining_txns;
  ulong curr_offset;

  ulong curr_txn_sz;
};

typedef struct fd_raw_block_txn_iter fd_raw_block_txn_iter_t;

/* The below logic is used to size out the memory footprint generated by the 
   runtime during transaction execution. */

/* The prevailing layout we have in the runtime is the meta followed by
   the account's data. This struct encodes that layout and asserts that
   the alignment requirements of the constituents are satisfied. */
// TODO: Use this struct at allocation sites so it's clear we use this layout
struct __attribute__((packed)) fd_account_rec {
  fd_account_meta_t meta;
  uchar data[];
};
typedef struct fd_account_rec fd_account_rec_t;
#define FD_ACCOUNT_REC_ALIGN      (8UL)
#define FD_ACCOUNT_REC_DATA_ALIGN (8UL)
FD_STATIC_ASSERT( FD_ACCOUNT_REC_ALIGN>=FD_ACCOUNT_META_ALIGN,     account_rec_meta_align );
FD_STATIC_ASSERT( FD_ACCOUNT_REC_ALIGN>=FD_ACCOUNT_REC_DATA_ALIGN, account_rec_data_align );
FD_STATIC_ASSERT( (offsetof(fd_account_rec_t, meta)%FD_ACCOUNT_META_ALIGN)==0,     account_rec_meta_offset );
FD_STATIC_ASSERT( (offsetof(fd_account_rec_t, data)%FD_ACCOUNT_REC_DATA_ALIGN)==0, account_rec_data_offset );

#define MAX_PERMITTED_DATA_INCREASE (10240UL) // 10KB
#define FD_BPF_ALIGN_OF_U128        (8UL    )
FD_STATIC_ASSERT( FD_BPF_ALIGN_OF_U128==FD_ACCOUNT_REC_DATA_ALIGN, input_data_align );
#define FD_RUNTIME_INPUT_REGION_ALLOC_ALIGN_UP (16UL)

/******** These macros bound out memory footprint ********/

/* The tight upper bound on borrowed account footprint over the
   execution of a single transaction. */
#define FD_RUNTIME_BORROWED_ACCOUNT_FOOTPRINT (MAX_TX_ACCOUNT_LOCKS * fd_ulong_align_up( FD_ACC_TOT_SZ_MAX, FD_ACCOUNT_REC_ALIGN ))

/* The tight-ish upper bound on input region footprint over the
   execution of a single transaction. See input serialization code for
   reference: fd_bpf_loader_serialization.c

   This bound is based off of the transaction MTU. We consider the
   question of what kind of transaction one would construct to
   maximally bloat the input region.
   The worst case scenario is when every nested instruction references
   all unique accounts in the transaction. A transaction can lock a max
   of MAX_TX_ACCOUNT_LOCKS accounts. Then all remaining input account
   references are going to be duplicates, which cost 1 byte to specify
   offset in payload, and which cost 8 bytes during serialization. Then
   there would be 0 bytes of instruction data, because they exist byte
   for byte in the raw payload, which is not a worthwhile bloat factor.
 */
#define FD_RUNTIME_INPUT_REGION_UNIQUE_ACCOUNT_FOOTPRINT(direct_mapping)                                                                                      \
                                                        (1UL                         /* dup byte          */                                                + \
                                                         sizeof(uchar)               /* is_signer         */                                                + \
                                                         sizeof(uchar)               /* is_writable       */                                                + \
                                                         sizeof(uchar)               /* executable        */                                                + \
                                                         sizeof(uint)                /* original_data_len */                                                + \
                                                         sizeof(fd_pubkey_t)         /* key               */                                                + \
                                                         sizeof(fd_pubkey_t)         /* owner             */                                                + \
                                                         sizeof(ulong)               /* lamports          */                                                + \
                                                         sizeof(ulong)               /* data len          */                                                + \
                                                         (direct_mapping ? FD_BPF_ALIGN_OF_U128 : fd_ulong_align_up( FD_ACC_SZ_MAX, FD_BPF_ALIGN_OF_U128 )) + \
                                                         MAX_PERMITTED_DATA_INCREASE                                                                        + \
                                                         sizeof(ulong))              /* rent_epoch        */

#define FD_RUNTIME_INPUT_REGION_INSN_FOOTPRINT(account_lock_limit, direct_mapping)                                                                       \
                                              (fd_ulong_align_up( (sizeof(ulong)         /* acct_cnt       */                                          + \
                                                                   account_lock_limit*FD_RUNTIME_INPUT_REGION_UNIQUE_ACCOUNT_FOOTPRINT(direct_mapping) + \
                                                                   sizeof(ulong)         /* instr data len */                                          + \
                                                                                         /* No instr data  */                                            \
                                                                   sizeof(fd_pubkey_t)), /* program id     */                                            \
                                                                   FD_RUNTIME_INPUT_REGION_ALLOC_ALIGN_UP ) + FD_BPF_ALIGN_OF_U128)

#define FD_RUNTIME_INPUT_REGION_TXN_FOOTPRINT(account_lock_limit, direct_mapping)                                                                           \
                                             ((FD_MAX_INSTRUCTION_STACK_DEPTH*FD_RUNTIME_INPUT_REGION_INSN_FOOTPRINT(account_lock_limit, direct_mapping)) + \
                                              ((FD_TXN_MTU-FD_TXN_MIN_SERIALIZED_SZ-account_lock_limit)*8UL)) /* We can have roughly this much duplicate offsets */

/* Bincode valloc footprint over the execution of a single transaction.
   As well as other footprint specific to each native program type.

   N.B. We know that bincode valloc footprint is bounded, because
   whenever we alloc something, we advance our pointer into the binary
   buffer, so eventually we are gonna reach the end of the buffer.
   This buffer is usually backed by and ultimately bounded in size by
   either accounts data or the transaction MTU.

   That being said, it's not obvious what the tight upper bound would
   be for allocations across all possible execution paths of all native
   programs, including possible CPIs from native programs.  The
   footprint estimate here is based on a manual review of our native
   program implementation.  Note that even if the possible paths remain
   steady at the Solana protocol level, the footprint is subject to
   change when we change our implementation.

   ### Native programs
   ALUT (migrated to BPF)
   Loader
     - rodata for bpf program relocation and validation
   Compute budget (0 allocations)
   Config (migrated to BPF)
   Precompile (0 allocations)
   Stake
     - The instruction with the largest footprint is deactivate_delinquent
       - During instruction decode, no allocations
       - During execution, this is (vote account get_state() + vote convert_to_current()) times 2, once for delinquent_vote_account, and once for reference_vote_account
   System
     - system_program_instruction_decode seed
   Vote
     - The instruction with the largest footprint is compact vote state update
       - During instruction decode, this is 9*lockouts_len bytes, MTU bounded
       - During execution, this is vote account get_state() + vote convert_to_current() + 12*lockouts_len bytes + lockouts_len ulong + deq_fd_landed_vote_t_alloc(lockouts_len)
   Zk Elgamal (0 allocations)

   The largest footprint is hence deactivate_delinquent, in which the
   two get_state() calls dominate the footprint.  In particular, the
   authorized_voters treaps bloat 40 bytes (epoch+pubkey) in a vote
   account to 72 bytes (sizeof(fd_vote_authorized_voter_t)) in memory.
 */
#define FD_RUNTIME_BINCODE_AND_NATIVE_FOOTPRINT (2UL*FD_ACC_SZ_MAX*72UL/40UL)

/* Misc other footprint. */
#define FD_RUNTIME_SYSCALL_TABLE_FOOTPRINT (FD_MAX_INSTRUCTION_STACK_DEPTH*fd_ulong_align_up( fd_sbpf_syscalls_footprint(), fd_sbpf_syscalls_align() ))

#ifdef FD_DEBUG_SBPF_TRACES
#define FD_RUNTIME_VM_TRACE_EVENT_MAX      (1UL<<30)
#define FD_RUNTIME_VM_TRACE_EVENT_DATA_MAX (2048UL)
#define FD_RUNTIME_VM_TRACE_FOOTPRINT (FD_MAX_INSTRUCTION_STACK_DEPTH*fd_ulong_align_up( fd_vm_trace_footprint( FD_RUNTIME_VM_TRACE_EVENT_MAX, FD_RUNTIME_VM_TRACE_EVENT_DATA_MAX ), fd_vm_trace_align() ))
#else
#define FD_RUNTIME_VM_TRACE_FOOTPRINT (0UL)
#endif

#define FD_RUNTIME_MISC_FOOTPRINT (FD_RUNTIME_SYSCALL_TABLE_FOOTPRINT+FD_RUNTIME_VM_TRACE_FOOTPRINT)

/* Now finally, we bound out the footprint of transaction execution. */
#define FD_RUNTIME_TRANSACTION_EXECUTION_FOOTPRINT(account_lock_limit, direct_mapping)                                         \
                                                  (FD_RUNTIME_BORROWED_ACCOUNT_FOOTPRINT                                     + \
                                                   FD_RUNTIME_INPUT_REGION_TXN_FOOTPRINT(account_lock_limit, direct_mapping) + \
                                                   FD_RUNTIME_BINCODE_AND_NATIVE_FOOTPRINT                                   + \
                                                   FD_RUNTIME_MISC_FOOTPRINT)

/* Convenience macros for common use cases. 
   
   TODO: If account lock limits are increased to 128, this macro will need to be updated. */
#define FD_RUNTIME_TRANSACTION_EXECUTION_FOOTPRINT_FUZZ    FD_RUNTIME_TRANSACTION_EXECUTION_FOOTPRINT(64UL, 0)
#define FD_RUNTIME_TRANSACTION_EXECUTION_FOOTPRINT_DEFAULT FD_RUNTIME_TRANSACTION_EXECUTION_FOOTPRINT(64UL, 0)

/* Helpers for runtime spad frame management. */
struct fd_runtime_spad_verify_handle_private {
  fd_spad_t *         spad;
  fd_exec_txn_ctx_t * txn_ctx;
};
typedef struct fd_runtime_spad_verify_handle_private fd_runtime_spad_verify_handle_private_t;

static inline void
fd_runtime_spad_private_frame_end( fd_runtime_spad_verify_handle_private_t * _spad_handle ) {
  /* fd_spad_verify() returns 0 if everything looks good, and non-zero
     otherwise.

     Since the fast spad alloc API doesn't check for or indicate an OOM
     situation and is going to happily permit an OOB alloc, we need
     some way of detecting that. Moreover, we would also like to detect
     unbalanced frame push/pop or usage of more frames than allowed.
     While surrounding the spad with guard regions will help detect the
     former, it won't necessarily catch the latter.

     On compliant transactions, fd_spad_verify() isn't all that
     expensive.  Nonetheless, We invoke fd_spad_verify() only at the
     peak of memory usage, and not gratuitously everywhere. One peak
     would be right before we do the most deeply nested spad frame pop.
     However, we do pops through compiler-inserted cleanup functions
     that take only a single pointer, so we define this helper function
     to access the needed context info.  The end result is that we do
     super fast spad calls everywhere in the runtime, and every now and
     then we invoke verify to check things. */
  /* -1UL because spad pop is called after instr stack pop. */
  if( FD_UNLIKELY( _spad_handle->txn_ctx->instr_stack_sz>=FD_MAX_INSTRUCTION_STACK_DEPTH-1UL && fd_spad_verify( _spad_handle->txn_ctx->spad ) ) ) {
    uchar const * txn_signature = (uchar const *)fd_txn_get_signatures( _spad_handle->txn_ctx->txn_descriptor, _spad_handle->txn_ctx->_txn_raw->raw );
    FD_BASE58_ENCODE_64_BYTES( txn_signature, sig );
    FD_LOG_ERR(( "spad corrupted or overflown on transaction %s", sig ));
  }
  fd_spad_pop( _spad_handle->spad );
}

#define FD_RUNTIME_TXN_SPAD_FRAME_BEGIN(_spad, _txn_ctx) do {                                                        \
  fd_runtime_spad_verify_handle_private_t _spad_handle __attribute__((cleanup(fd_runtime_spad_private_frame_end))) = \
    (fd_runtime_spad_verify_handle_private_t) { .spad = _spad, .txn_ctx = _txn_ctx };                                \
  fd_spad_push( _spad_handle.spad );                                                                                 \
  do

#define FD_RUNTIME_TXN_SPAD_FRAME_END while(0); } while(0)

FD_PROTOTYPES_BEGIN

/* Runtime Helpers ************************************************************/

void
fd_runtime_update_leaders( fd_exec_slot_ctx_t * slot_ctx, ulong slot );

int
fd_runtime_sysvar_cache_load( fd_exec_slot_ctx_t * slot_ctx );

/* TODO: Invoked by fd_executor: layering violation. Rent logic is deprecated
   and will be torn out entirely very soon. */
ulong
fd_runtime_collect_rent_from_account( fd_exec_slot_ctx_t const * slot_ctx,
                                      fd_account_meta_t  *       acc,
                                      fd_pubkey_t const  *       key,
                                      ulong                      epoch );

/* Block Level Execution Prep/Finalize ****************************************/

int
fd_runtime_block_execute_prepare( fd_exec_slot_ctx_t * slot_ctx );

int
fd_runtime_block_execute_finalize_tpool( fd_exec_slot_ctx_t *    slot_ctx,
                                         fd_capture_ctx_t *      capture_ctx,
                                         fd_block_info_t const * block_info,
                                         fd_tpool_t *            tpool );

/* Transaction Level Execution Management *************************************/

/* fd_runtime_prepare_txns_start and fd_runtime_pre_execute_check are only
   publicly exposed for the fuzzing harnesses. These functions are responsible 
   for various transaction sanitization checks. */

int
fd_runtime_prepare_txns_start( fd_exec_slot_ctx_t *         slot_ctx,
                               fd_execute_txn_task_info_t * task_info,
                               fd_txn_p_t *                 txns,
                               ulong                        txn_cnt );

void
fd_runtime_pre_execute_check( fd_execute_txn_task_info_t * task_info );

/* fd_runtime_process_txns is responsible for end-to-end preparing, executing,
   and finalizing a list of transactions. It will execute all of the
   transactions on a single core. */
int
fd_runtime_process_txns( fd_exec_slot_ctx_t * slot_ctx,
                         fd_spad_t *          spad,
                         fd_capture_ctx_t *   capture_ctx,
                         fd_txn_p_t *         txns,
                         ulong                txn_cnt );


/* fd_runtime_execute_txns_in_waves_tpool is responsible for end-to-end
   preparing, executing and finalizng a list of transactions. It will schedule
   out a set of transactions to maximize parallelism*/
int
fd_runtime_process_txns_in_waves_tpool( fd_exec_slot_ctx_t * slot_ctx,
                                        fd_capture_ctx_t *   capture_ctx,
                                        fd_txn_p_t *         txns,
                                        ulong                txn_cnt,
                                        fd_tpool_t *         tpool,
                                        fd_spad_t * *        spads, 
                                        ulong                spads_cnt );

/* fd_runtime_process_txns and fd_runtime_execute_txns_in_waves_tpool are 
   both entrypoints for executing transactions. Currently, the former is used
   in the leader pipeline as conflict-free microblocks are streamed in from the 
   pack tile. The latter is used for replaying non-leader blocks. Currently the
   entire block must be recieved to start replaying. This allows us to scheedule 
   out all of the transactions. Eventually, transactions will be executed in 
   a streamed fashion.*/

/* Epoch Boundary *************************************************************/

uint
fd_runtime_is_epoch_boundary( fd_epoch_bank_t * epoch_bank,
                              ulong             curr_slot,
                              ulong             prev_slot );

void
fd_process_new_epoch( fd_exec_slot_ctx_t * slot_ctx,
                      ulong                parent_epoch );

/* Debugging Tools ************************************************************/

void
fd_runtime_checkpt( fd_capture_ctx_t * capture_ctx,
                    fd_exec_slot_ctx_t * slot_ctx,
                    ulong slot );

/* TODO: This logic is very old and likely needs to be reworked to work with the
   snapshot service. It is not being removed as the logic can probably reused
   for snapshot minimization. */

void
fd_runtime_collect_rent_accounts_prune( ulong slot,
                                        fd_exec_slot_ctx_t * slot_ctx,
                                        fd_capture_ctx_t * capture_ctx );

/* Block Parsing **************************************************************/

/* Live Replay APIs */

fd_raw_block_txn_iter_t
fd_raw_block_txn_iter_init( uchar const *                  orig_data,
                            fd_block_entry_batch_t const * batches,
                            ulong                          batch_cnt );

ulong
fd_raw_block_txn_iter_done( fd_raw_block_txn_iter_t iter );

fd_raw_block_txn_iter_t
fd_raw_block_txn_iter_next( fd_raw_block_txn_iter_t iter );

void
fd_raw_block_txn_iter_ele( fd_raw_block_txn_iter_t iter, fd_txn_p_t * out_txn );

/* Offline Replay *************************************************************/

int
fd_runtime_block_eval_tpool( fd_exec_slot_ctx_t * slot_ctx,
                             fd_capture_ctx_t *   capture_ctx,
                             fd_tpool_t *         tpool,
                             ulong                scheduler,
                             ulong *              txn_cnt,
                             fd_spad_t * *        spads,
                             ulong                spads_cnt );

/* Genesis ********************************************************************/

void
fd_runtime_read_genesis( fd_exec_slot_ctx_t * slot_ctx,
                         char const *         genesis_filepath,
                         uchar                is_snapshot,
                         fd_capture_ctx_t   * capture_ctx,
                         fd_tpool_t *         tpool );

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_flamenco_runtime_fd_runtime_h */
