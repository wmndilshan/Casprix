/*
 * LLM Runtime - Transformer Backward Pass
 *
 * Implements transformer_backward() using the autograd tape.
 * Re-runs the forward pass with tracked operations, then calls
 * ai_tape_backward() to compute all gradients automatically.
 *
 * Parameter gradient ordering is part of the transformer training ABI:
 *   [0] token_embedding
 *   [1] pos_embedding
 *   Per layer (12 each):
 *     [2+i*12+0..3] Wq, Wk, Wv, Wo
 *     [2+i*12+4..7] W1, b1, W2, b2
 *     [2+i*12+8..11] ln1_gamma, ln1_beta, ln2_gamma, ln2_beta
 *   Final:
 *     [2+L*12+0] ln_final_gamma
 *     [2+L*12+1] ln_final_beta
 *     [2+L*12+2] lm_head
 */

#include "../engine/tape.h"
#include "transformer.h"
#include "training.h"
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Autograd-traced forward pass for a single transformer block
 * ==========================================================================*/

static AiTapeValue* ai_tape_transformer_block_forward(
    AiTape* tape, AiTapeValue* x,
    AiTapeValue* ln1_gamma, AiTapeValue* ln1_beta,
    AiTapeValue* Wq, AiTapeValue* Wk, AiTapeValue* Wv, AiTapeValue* Wo,
    AiTapeValue* ln2_gamma, AiTapeValue* ln2_beta,
    AiTapeValue* W1, AiTapeValue* b1, AiTapeValue* W2, AiTapeValue* b2,
    i32 batch, i32 seq_len, i32 hidden_dim,
    i32 num_heads, i32 head_dim, i32 ffn_dim)
{
    i32 BS = batch * seq_len;

    /* === Pre-attention LayerNorm === */
    AiTapeValue* ln1_out = ai_tape_layernorm(tape, x, ln1_gamma, ln1_beta,
                                        BS, hidden_dim, 1e-5f);

    /* === Multi-head Self-Attention === */
    AiTapeValue* attn_out = ai_tape_attention(tape, ln1_out, Wq, Wk, Wv, Wo,
                                         batch, seq_len, num_heads, head_dim);

    /* === Residual connection 1 === */
    AiTapeValue* res1 = ai_tape_add(tape, x, attn_out);

    /* === Pre-FFN LayerNorm === */
    AiTapeValue* ln2_out = ai_tape_layernorm(tape, res1, ln2_gamma, ln2_beta,
                                        BS, hidden_dim, 1e-5f);

    /* === Feed-Forward Network === */
    /* hidden = GELU(ln2_out * W1 + b1) */
    AiTapeValue* ffn_hidden = ai_tape_gemm_bias(tape, ln2_out, W1, b1,
                                           BS, hidden_dim, ffn_dim);
    AiTapeValue* ffn_act = ai_tape_gelu(tape, ffn_hidden);

    /* output = ffn_act * W2 + b2 */
    AiTapeValue* ffn_out = ai_tape_gemm_bias(tape, ffn_act, W2, b2,
                                        BS, ffn_dim, hidden_dim);

    /* === Residual connection 2 === */
    AiTapeValue* res2 = ai_tape_add(tape, res1, ffn_out);

    return res2;
}

/* ============================================================================
 * transformer_backward - Full backward pass using autograd
 * ==========================================================================*/

void transformer_backward(TransformerModel* model, const Tensor* logits,
                           const i32* token_ids, const i32* targets,
                           i32 batch, i32 seq_len,
                           Tensor** param_grads, MemoryPools* mem) {
    i32 hidden_dim = model->hidden_dim;
    i32 vocab_size = model->vocab_size;
    i32 num_layers = model->num_layers;
    i32 BS = batch * seq_len;

    /* Create autograd tape on the grad_cache arena */
    AiTape* tape = ai_tape_create(mem->grad_cache);
    if (!tape) {
        fprintf(stderr, "ERROR: Failed to create autograd tape\n");
        return;
    }
    ai_tape_begin(tape);

    /* ================================================================
     * Wrap all model parameters as GradTensors
     * Ordering must match the public parameter layout documented above.
     * ================================================================ */

    /* Embeddings */
    AiTapeValue* gt_tok_emb = ai_tape_parameter(tape, model->embeddings->token_embedding);
    AiTapeValue* gt_pos_emb = ai_tape_parameter(tape, model->embeddings->pos_embedding);
    (void)gt_pos_emb;

    /* Per-layer parameters */
    AiTapeValue** gt_Wq = (AiTapeValue**)tensor_arena_alloc(mem->grad_cache,
        num_layers * sizeof(AiTapeValue*), 8);
    AiTapeValue** gt_Wk = (AiTapeValue**)tensor_arena_alloc(mem->grad_cache,
        num_layers * sizeof(AiTapeValue*), 8);
    AiTapeValue** gt_Wv = (AiTapeValue**)tensor_arena_alloc(mem->grad_cache,
        num_layers * sizeof(AiTapeValue*), 8);
    AiTapeValue** gt_Wo = (AiTapeValue**)tensor_arena_alloc(mem->grad_cache,
        num_layers * sizeof(AiTapeValue*), 8);
    AiTapeValue** gt_W1 = (AiTapeValue**)tensor_arena_alloc(mem->grad_cache,
        num_layers * sizeof(AiTapeValue*), 8);
    AiTapeValue** gt_b1 = (AiTapeValue**)tensor_arena_alloc(mem->grad_cache,
        num_layers * sizeof(AiTapeValue*), 8);
    AiTapeValue** gt_W2 = (AiTapeValue**)tensor_arena_alloc(mem->grad_cache,
        num_layers * sizeof(AiTapeValue*), 8);
    AiTapeValue** gt_b2 = (AiTapeValue**)tensor_arena_alloc(mem->grad_cache,
        num_layers * sizeof(AiTapeValue*), 8);
    AiTapeValue** gt_ln1g = (AiTapeValue**)tensor_arena_alloc(mem->grad_cache,
        num_layers * sizeof(AiTapeValue*), 8);
    AiTapeValue** gt_ln1b = (AiTapeValue**)tensor_arena_alloc(mem->grad_cache,
        num_layers * sizeof(AiTapeValue*), 8);
    AiTapeValue** gt_ln2g = (AiTapeValue**)tensor_arena_alloc(mem->grad_cache,
        num_layers * sizeof(AiTapeValue*), 8);
    AiTapeValue** gt_ln2b = (AiTapeValue**)tensor_arena_alloc(mem->grad_cache,
        num_layers * sizeof(AiTapeValue*), 8);

    for (i32 i = 0; i < num_layers; i++) {
        TransformerBlock* block = model->blocks[i];
        gt_Wq[i] = ai_tape_parameter(tape, block->attn->Wq);
        gt_Wk[i] = ai_tape_parameter(tape, block->attn->Wk);
        gt_Wv[i] = ai_tape_parameter(tape, block->attn->Wv);
        gt_Wo[i] = ai_tape_parameter(tape, block->attn->Wo);
        gt_W1[i] = ai_tape_parameter(tape, block->ffn->W1);
        gt_b1[i] = ai_tape_parameter(tape, block->ffn->b1);
        gt_W2[i] = ai_tape_parameter(tape, block->ffn->W2);
        gt_b2[i] = ai_tape_parameter(tape, block->ffn->b2);
        gt_ln1g[i] = ai_tape_parameter(tape, block->ln1_gamma);
        gt_ln1b[i] = ai_tape_parameter(tape, block->ln1_beta);
        gt_ln2g[i] = ai_tape_parameter(tape, block->ln2_gamma);
        gt_ln2b[i] = ai_tape_parameter(tape, block->ln2_beta);
    }

    /* Final LayerNorm + LM head */
    AiTapeValue* gt_ln_final_g = ai_tape_parameter(tape, model->ln_final_gamma);
    AiTapeValue* gt_ln_final_b = ai_tape_parameter(tape, model->ln_final_beta);
    AiTapeValue* gt_lm_head = ai_tape_parameter(tape, model->lm_head);

    /* ================================================================
     * Re-run forward pass on tape (records all ops)
     * ================================================================ */

    /* Embedding: token + positional */
    AiTapeValue* tok_emb_out = ai_tape_embedding(tape, gt_tok_emb, token_ids,
                                            batch, seq_len, hidden_dim);

    /* Add positional embeddings manually */
    /* pos_embedding is [max_seq_len, hidden_dim], we need [seq_len, hidden_dim] */
    /* Create a view/slice of pos_embedding for current seq_len */
    Tensor* pos_slice = tensor_arena_alloc_tensor(mem->grad_cache, 2,
                                            (i32[]){BS, hidden_dim});
    for (i32 b = 0; b < batch; b++) {
        for (i32 s = 0; s < seq_len; s++) {
            memcpy(pos_slice->data + (b * seq_len + s) * hidden_dim,
                   model->embeddings->pos_embedding->data + s * hidden_dim,
                   hidden_dim * sizeof(f32));
        }
    }
    AiTapeValue* gt_pos_slice = ai_tape_parameter(tape, pos_slice);
    /* We track pos_slice but will map its gradients back to pos_embedding */

    AiTapeValue* x = ai_tape_add(tape, tok_emb_out, gt_pos_slice);

    /* Transformer blocks */
    i32 num_heads = model->blocks[0]->attn->num_heads;
    i32 head_dim = model->blocks[0]->attn->head_dim;
    i32 ffn_dim = model->blocks[0]->ffn->W1->shape[1];

    for (i32 i = 0; i < num_layers; i++) {
        x = ai_tape_transformer_block_forward(tape, x,
            gt_ln1g[i], gt_ln1b[i],
            gt_Wq[i], gt_Wk[i], gt_Wv[i], gt_Wo[i],
            gt_ln2g[i], gt_ln2b[i],
            gt_W1[i], gt_b1[i], gt_W2[i], gt_b2[i],
            batch, seq_len, hidden_dim, num_heads, head_dim, ffn_dim);
    }

    /* Final LayerNorm */
    AiTapeValue* ln_final = ai_tape_layernorm(tape, x, gt_ln_final_g, gt_ln_final_b,
                                         BS, hidden_dim, 1e-5f);

    /* LM head: logits = ln_final * lm_head */
    AiTapeValue* ai_tape_logits = ai_tape_gemm(tape, ln_final, gt_lm_head,
                                     BS, hidden_dim, vocab_size);

    /* Cross-entropy loss */
    AiTapeValue* loss = ai_tape_cross_entropy(tape, ai_tape_logits, targets, BS, vocab_size);
    (void)loss;

    /* ================================================================
     * Backward pass
     * ================================================================ */
    ai_tape_end(tape);
    ai_tape_backward(tape);

    /* ================================================================
     * Extract gradients into param_grads array
     * Ordering: same as collect_parameters()
     * ================================================================ */

    i32 idx = 0;

    /* [0] token_embedding */
    if (gt_tok_emb->grad && param_grads[idx]) {
        memcpy(param_grads[idx]->data, gt_tok_emb->grad->data,
               gt_tok_emb->grad->size * sizeof(f32));
    }
    idx++;

    /* [1] pos_embedding */
    if (gt_pos_slice->grad && param_grads[idx]) {
        /* Accumulate pos_slice grads back to full pos_embedding grad */
        /* pos_slice was [BS, hidden_dim], pos_embedding is [max_seq_len, hidden_dim] */
        tensor_zeros(param_grads[idx]);
        for (i32 b = 0; b < batch; b++) {
            for (i32 s = 0; s < seq_len; s++) {
                f32* grad_src = gt_pos_slice->grad->data + (b * seq_len + s) * hidden_dim;
                f32* grad_dst = param_grads[idx]->data + s * hidden_dim;
                for (i32 d = 0; d < hidden_dim; d++) {
                    grad_dst[d] += grad_src[d];
                }
            }
        }
    }
    idx++;

    /* Per-layer parameters (12 per layer) */
    for (i32 i = 0; i < num_layers; i++) {
        /* Attention: Wq, Wk, Wv, Wo */
        AiTapeValue* attn_gts[] = { gt_Wq[i], gt_Wk[i], gt_Wv[i], gt_Wo[i] };
        for (i32 j = 0; j < 4; j++) {
            if (attn_gts[j]->grad && param_grads[idx]) {
                memcpy(param_grads[idx]->data, attn_gts[j]->grad->data,
                       attn_gts[j]->grad->size * sizeof(f32));
            }
            idx++;
        }

        /* FFN: W1, b1, W2, b2 */
        AiTapeValue* ffn_gts[] = { gt_W1[i], gt_b1[i], gt_W2[i], gt_b2[i] };
        for (i32 j = 0; j < 4; j++) {
            if (ffn_gts[j]->grad && param_grads[idx]) {
                memcpy(param_grads[idx]->data, ffn_gts[j]->grad->data,
                       ffn_gts[j]->grad->size * sizeof(f32));
            }
            idx++;
        }

        /* LayerNorm: ln1_gamma, ln1_beta, ln2_gamma, ln2_beta */
        AiTapeValue* ln_gts[] = { gt_ln1g[i], gt_ln1b[i], gt_ln2g[i], gt_ln2b[i] };
        for (i32 j = 0; j < 4; j++) {
            if (ln_gts[j]->grad && param_grads[idx]) {
                memcpy(param_grads[idx]->data, ln_gts[j]->grad->data,
                       ln_gts[j]->grad->size * sizeof(f32));
            }
            idx++;
        }
    }

    /* Final: ln_final_gamma, ln_final_beta, lm_head */
    if (gt_ln_final_g->grad && param_grads[idx]) {
        memcpy(param_grads[idx]->data, gt_ln_final_g->grad->data,
               gt_ln_final_g->grad->size * sizeof(f32));
    }
    idx++;

    if (gt_ln_final_b->grad && param_grads[idx]) {
        memcpy(param_grads[idx]->data, gt_ln_final_b->grad->data,
               gt_ln_final_b->grad->size * sizeof(f32));
    }
    idx++;

    if (gt_lm_head->grad && param_grads[idx]) {
        memcpy(param_grads[idx]->data, gt_lm_head->grad->data,
               gt_lm_head->grad->size * sizeof(f32));
    }
}
