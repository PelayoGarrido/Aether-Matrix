# Copyright 2025 Google Inc. HuggingFace Inc. team. All rights reserved.
# Licensed under the Apache License, Version 2.0.
# http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an AS IS BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
# Selected unmodified definitions from Transformers v4.54.0 modeling_t5gemma.py.
# Adaptation: imports limited to torch; inference-only module/activation aliases.
# Used only for independent eager-layer numerical validation, not generation.
from __future__ import annotations
from typing import Callable, Optional
import torch
from torch import nn
GradientCheckpointingLayer = nn.Module
ACT2FN = {"gelu_pytorch_tanh": lambda x: nn.functional.gelu(x, approximate="tanh")}

class T5GemmaRMSNorm(nn.Module):
    def __init__(self, dim: int, eps: float = 1e-6):
        super().__init__()
        self.eps = eps
        self.weight = nn.Parameter(torch.zeros(dim))

    def _norm(self, x):
        return x * torch.rsqrt(x.pow(2).mean(-1, keepdim=True) + self.eps)

    def forward(self, x):
        output = self._norm(x.float())
        # Llama does x.to(float16) * w whilst T5Gemma is (x * w).to(float16)
        # See https://github.com/huggingface/transformers/pull/29402
        output = output * (1.0 + self.weight.float())
        return output.type_as(x)

    def extra_repr(self):
        return f"{tuple(self.weight.shape)}, eps={self.eps}"

class T5GemmaMLP(nn.Module):
    def __init__(self, config):
        super().__init__()
        self.config = config
        self.hidden_size = config.hidden_size
        self.intermediate_size = config.intermediate_size
        self.gate_proj = nn.Linear(self.hidden_size, self.intermediate_size, bias=False)
        self.up_proj = nn.Linear(self.hidden_size, self.intermediate_size, bias=False)
        self.down_proj = nn.Linear(self.intermediate_size, self.hidden_size, bias=False)
        self.act_fn = ACT2FN[config.hidden_activation]
        self.dropout = nn.Dropout(config.dropout_rate)

    def forward(self, x):
        hidden_states = self.act_fn(self.gate_proj(x)) * self.up_proj(x)
        hidden_states = self.dropout(hidden_states)
        down_proj = self.down_proj(hidden_states)
        return down_proj

def rotate_half(x):
    """Rotates half the hidden dims of the input."""
    x1 = x[..., : x.shape[-1] // 2]
    x2 = x[..., x.shape[-1] // 2 :]
    return torch.cat((-x2, x1), dim=-1)

def apply_rotary_pos_emb(q, k, cos, sin, position_ids=None, unsqueeze_dim=1):
    """Applies Rotary Position Embedding to the query and key tensors.

    Args:
        q (`torch.Tensor`): The query tensor.
        k (`torch.Tensor`): The key tensor.
        cos (`torch.Tensor`): The cosine part of the rotary embedding.
        sin (`torch.Tensor`): The sine part of the rotary embedding.
        position_ids (`torch.Tensor`, *optional*):
            Deprecated and unused.
        unsqueeze_dim (`int`, *optional*, defaults to 1):
            The 'unsqueeze_dim' argument specifies the dimension along which to unsqueeze cos[position_ids] and
            sin[position_ids] so that they can be properly broadcasted to the dimensions of q and k. For example, note
            that cos[position_ids] and sin[position_ids] have the shape [batch_size, seq_len, head_dim]. Then, if q and
            k have the shape [batch_size, heads, seq_len, head_dim], then setting unsqueeze_dim=1 makes
            cos[position_ids] and sin[position_ids] broadcastable to the shapes of q and k. Similarly, if q and k have
            the shape [batch_size, seq_len, heads, head_dim], then set unsqueeze_dim=2.
    Returns:
        `tuple(torch.Tensor)` comprising of the query and key tensors rotated using the Rotary Position Embedding.
    """
    cos = cos.unsqueeze(unsqueeze_dim)
    sin = sin.unsqueeze(unsqueeze_dim)
    q_embed = (q * cos) + (rotate_half(q) * sin)
    k_embed = (k * cos) + (rotate_half(k) * sin)
    return q_embed, k_embed

def repeat_kv(hidden_states: torch.Tensor, n_rep: int) -> torch.Tensor:
    """
    This is the equivalent of torch.repeat_interleave(x, dim=1, repeats=n_rep). The hidden states go from (batch,
    num_key_value_heads, seqlen, head_dim) to (batch, num_attention_heads, seqlen, head_dim)
    """
    batch, num_key_value_heads, slen, head_dim = hidden_states.shape
    if n_rep == 1:
        return hidden_states
    hidden_states = hidden_states[:, :, None, :, :].expand(batch, num_key_value_heads, n_rep, slen, head_dim)
    return hidden_states.reshape(batch, num_key_value_heads * n_rep, slen, head_dim)

def eager_attention_forward(
    module: nn.Module,
    query: torch.Tensor,
    key: torch.Tensor,
    value: torch.Tensor,
    attention_mask: Optional[torch.Tensor],
    dropout: float = 0.0,
    scaling: Optional[float] = None,
    softcap: Optional[float] = None,
    **kwargs,
) -> tuple[torch.Tensor, torch.Tensor]:
    if scaling is None:
        scaling = module.head_dim**-0.5

    key_states = repeat_kv(key, module.num_key_value_groups)
    value_states = repeat_kv(value, module.num_key_value_groups)

    attn_weights = torch.matmul(query, key_states.transpose(2, 3)) * scaling

    if softcap is not None:
        attn_weights = attn_weights / softcap
        attn_weights = torch.tanh(attn_weights)
        attn_weights = attn_weights * softcap
    if attention_mask is not None:  # no matter the length, we just slice it
        causal_mask = attention_mask[:, :, :, : key_states.shape[-2]]
        attn_weights = attn_weights + causal_mask

    # upcast attention to fp32
    attn_weights = nn.functional.softmax(attn_weights, dim=-1, dtype=torch.float32).to(query.dtype)
    attn_weights = nn.functional.dropout(attn_weights, p=dropout, training=module.training)
    attn_output = torch.matmul(attn_weights, value_states)
    attn_output = attn_output.transpose(1, 2).contiguous()
    return attn_output, attn_weights

class T5GemmaSelfAttention(nn.Module):
    """Multi-headed attention from 'Attention Is All You Need' paper"""

    def __init__(self, config: T5GemmaModuleConfig, layer_idx: int):
        super().__init__()
        self.config = config
        self.layer_idx = layer_idx
        self.head_dim = getattr(config, "head_dim", config.hidden_size // config.num_attention_heads)
        self.num_key_value_groups = config.num_attention_heads // config.num_key_value_heads
        self.scaling = config.query_pre_attn_scalar**-0.5
        self.attention_dropout = self.config.attention_dropout
        # Requied by flash attention: encoder selfattention is non-causal
        self.is_causal = config.is_decoder

        self.q_proj = nn.Linear(
            config.hidden_size, config.num_attention_heads * self.head_dim, bias=config.attention_bias
        )
        self.k_proj = nn.Linear(
            config.hidden_size, config.num_key_value_heads * self.head_dim, bias=config.attention_bias
        )
        self.v_proj = nn.Linear(
            config.hidden_size, config.num_key_value_heads * self.head_dim, bias=config.attention_bias
        )
        self.o_proj = nn.Linear(
            config.num_attention_heads * self.head_dim, config.hidden_size, bias=config.attention_bias
        )
        self.attn_logit_softcapping = self.config.attn_logit_softcapping
        self.sliding_window = config.sliding_window if config.layer_types[layer_idx] == "sliding_attention" else None

    def forward(
        self,
        hidden_states: torch.Tensor,
        position_embeddings: tuple[torch.Tensor, torch.Tensor],
        attention_mask: Optional[torch.Tensor],
        past_key_value: Optional[Cache] = None,
        cache_position: Optional[torch.LongTensor] = None,
        **kwargs: Unpack[FlashAttentionKwargs],
    ) -> tuple[torch.Tensor, Optional[torch.Tensor], Optional[tuple[torch.Tensor]]]:
        input_shape = hidden_states.shape[:-1]
        hidden_shape = (*input_shape, -1, self.head_dim)

        query_states = self.q_proj(hidden_states).view(hidden_shape).transpose(1, 2)
        key_states = self.k_proj(hidden_states).view(hidden_shape).transpose(1, 2)
        value_states = self.v_proj(hidden_states).view(hidden_shape).transpose(1, 2)

        cos, sin = position_embeddings
        query_states, key_states = apply_rotary_pos_emb(query_states, key_states, cos, sin)

        if past_key_value is not None:
            # sin and cos are specific to RoPE models; cache_position needed for the static cache
            cache_kwargs = {"sin": sin, "cos": cos, "cache_position": cache_position}
            key_states, value_states = past_key_value.update(key_states, value_states, self.layer_idx, cache_kwargs)

        attention_interface: Callable = eager_attention_forward
        if self.config._attn_implementation != "eager":
            attention_interface = ALL_ATTENTION_FUNCTIONS[self.config._attn_implementation]

        attn_output, attn_weights = attention_interface(
            self,
            query_states,
            key_states,
            value_states,
            attention_mask,
            dropout=self.attention_dropout if self.training else 0.0,
            scaling=self.scaling,
            sliding_window=self.sliding_window,
            softcap=self.attn_logit_softcapping,
            **kwargs,
        )

        attn_output = attn_output.reshape(*input_shape, -1).contiguous()
        attn_output = self.o_proj(attn_output)
        return attn_output, attn_weights

class T5GemmaCrossAttention(nn.Module):
    """Multi-headed attention from 'Attention Is All You Need' paper"""

    def __init__(self, config: T5GemmaModuleConfig, layer_idx: int):
        super().__init__()
        self.config = config
        self.layer_idx = layer_idx
        self.head_dim = getattr(config, "head_dim", config.hidden_size // config.num_attention_heads)
        self.num_key_value_groups = config.num_attention_heads // config.num_key_value_heads
        self.scaling = config.query_pre_attn_scalar**-0.5
        self.attention_dropout = self.config.attention_dropout
        self.is_causal = False

        self.q_proj = nn.Linear(
            config.hidden_size, config.num_attention_heads * self.head_dim, bias=config.attention_bias
        )

        self.k_proj = nn.Linear(
            config.cross_attention_hidden_size, config.num_key_value_heads * self.head_dim, bias=config.attention_bias
        )
        self.v_proj = nn.Linear(
            config.cross_attention_hidden_size, config.num_key_value_heads * self.head_dim, bias=config.attention_bias
        )
        self.o_proj = nn.Linear(
            config.num_attention_heads * self.head_dim, config.hidden_size, bias=config.attention_bias
        )
        self.attn_logit_softcapping = self.config.attn_logit_softcapping

        if config.cross_attention_hidden_size is None:
            raise ValueError("Cross-attention needs cross_attention_hidden_size to be specified.")

    def forward(
        self,
        hidden_states: torch.Tensor,
        attention_mask: Optional[torch.Tensor],
        encoder_hidden_states: Optional[torch.Tensor],
        past_key_value: Optional[Cache] = None,
        **kwargs: Unpack[FlashAttentionKwargs],
    ) -> tuple[torch.Tensor, Optional[torch.Tensor], Optional[tuple[torch.Tensor]]]:
        if encoder_hidden_states is None:
            raise ValueError("Encoder hidden state is required for cross attention.")

        input_shape = hidden_states.shape[:-1]
        hidden_shape = (*input_shape, -1, self.head_dim)
        query_states = self.q_proj(hidden_states).view(hidden_shape).transpose(1, 2)

        if past_key_value is not None:
            is_updated = past_key_value.is_updated.get(self.layer_idx)
            curr_past_key_value = past_key_value.cross_attention_cache

        if past_key_value is None or not is_updated:
            encoder_input_shape = encoder_hidden_states.shape[:-1]
            encoder_hidden_shape = (*encoder_input_shape, -1, self.head_dim)
            key_states = self.k_proj(encoder_hidden_states).view(encoder_hidden_shape).transpose(1, 2)
            value_states = self.v_proj(encoder_hidden_states).view(encoder_hidden_shape).transpose(1, 2)

            if past_key_value is not None:
                key_states, value_states = curr_past_key_value.update(key_states, value_states, self.layer_idx)
                past_key_value.is_updated[self.layer_idx] = True
        else:
            key_states = curr_past_key_value.layers[self.layer_idx].keys
            value_states = curr_past_key_value.layers[self.layer_idx].values

        attention_interface: Callable = eager_attention_forward
        if self.config._attn_implementation != "eager":
            attention_interface = ALL_ATTENTION_FUNCTIONS[self.config._attn_implementation]

        attn_output, attn_weights = attention_interface(
            self,
            query_states,
            key_states,
            value_states,
            attention_mask,
            dropout=self.attention_dropout if self.training else 0.0,
            scaling=self.scaling,
            sliding_window=None,
            softcap=self.attn_logit_softcapping,
            **kwargs,
        )

        attn_output = attn_output.reshape(*input_shape, -1).contiguous()
        attn_output = self.o_proj(attn_output)
        return attn_output, attn_weights

class T5GemmaEncoderLayer(GradientCheckpointingLayer):
    """Encoder sub-layer."""

    def __init__(self, config, layer_idx: int):
        super().__init__()
        self.hidden_size = config.hidden_size
        self.config = config
        self.layer_idx = layer_idx
        self.attention_type = config.layer_types[layer_idx]

        self.self_attn = T5GemmaSelfAttention(
            config=config,
            layer_idx=layer_idx,
        )
        self.pre_self_attn_layernorm = T5GemmaRMSNorm(config.hidden_size, eps=config.rms_norm_eps)
        self.post_self_attn_layernorm = T5GemmaRMSNorm(config.hidden_size, eps=config.rms_norm_eps)

        self.mlp = T5GemmaMLP(config)
        self.pre_feedforward_layernorm = T5GemmaRMSNorm(config.hidden_size, eps=config.rms_norm_eps)
        self.post_feedforward_layernorm = T5GemmaRMSNorm(config.hidden_size, eps=config.rms_norm_eps)

        self.dropout = nn.Dropout(config.dropout_rate)

    def forward(
        self,
        hidden_states: torch.Tensor,
        position_embeddings: tuple[torch.Tensor, torch.Tensor],
        attention_mask: Optional[torch.Tensor] = None,
        position_ids: Optional[torch.LongTensor] = None,
        **kwargs,
    ) -> tuple[torch.FloatTensor,]:
        residual = hidden_states
        hidden_states = self.pre_self_attn_layernorm(hidden_states)
        hidden_states, _ = self.self_attn(
            hidden_states=hidden_states,
            position_embeddings=position_embeddings,
            attention_mask=attention_mask,
            position_ids=position_ids,
            past_key_value=None,
            **kwargs,
        )
        hidden_states = self.post_self_attn_layernorm(hidden_states)
        hidden_states = residual + self.dropout(hidden_states)

        residual = hidden_states
        hidden_states = self.pre_feedforward_layernorm(hidden_states)
        hidden_states = self.mlp(hidden_states)
        hidden_states = self.post_feedforward_layernorm(hidden_states)
        hidden_states = residual + self.dropout(hidden_states)
        return hidden_states

class T5GemmaDecoderLayer(T5GemmaEncoderLayer):
    """Decoder sub-layer: an extra cross-attention layer."""

    def __init__(self, config, layer_idx: int):
        super().__init__(config, layer_idx)
        self.cross_attn = T5GemmaCrossAttention(config=config, layer_idx=layer_idx)
        self.pre_cross_attn_layernorm = T5GemmaRMSNorm(config.hidden_size, eps=config.rms_norm_eps)
        self.post_cross_attn_layernorm = T5GemmaRMSNorm(config.hidden_size, eps=config.rms_norm_eps)

    def forward(
        self,
        hidden_states: torch.Tensor,
        position_embeddings: tuple[torch.Tensor, torch.Tensor],
        attention_mask: Optional[torch.Tensor] = None,
        position_ids: Optional[torch.LongTensor] = None,
        past_key_value: Optional[EncoderDecoderCache] = None,
        use_cache: Optional[bool] = False,
        cache_position: Optional[torch.LongTensor] = None,
        encoder_hidden_states: Optional[torch.Tensor] = None,
        encoder_attention_mask: Optional[torch.Tensor] = None,
        **kwargs,
    ) -> torch.FloatTensor:
        residual = hidden_states
        hidden_states = self.pre_self_attn_layernorm(hidden_states)
        hidden_states, _ = self.self_attn(
            hidden_states=hidden_states,
            position_embeddings=position_embeddings,
            attention_mask=attention_mask,
            position_ids=position_ids,
            past_key_value=past_key_value.self_attention_cache if past_key_value is not None else None,
            use_cache=use_cache,
            cache_position=cache_position,
            **kwargs,
        )
        hidden_states = self.post_self_attn_layernorm(hidden_states)
        hidden_states = residual + self.dropout(hidden_states)

        residual = hidden_states
        hidden_states = self.pre_cross_attn_layernorm(hidden_states)
        hidden_states, _ = self.cross_attn(
            hidden_states=hidden_states,
            encoder_hidden_states=encoder_hidden_states,
            attention_mask=encoder_attention_mask,
            past_key_value=past_key_value,
            use_cache=use_cache,
            **kwargs,
        )
        hidden_states = self.post_cross_attn_layernorm(hidden_states)
        hidden_states = residual + self.dropout(hidden_states)

        residual = hidden_states
        hidden_states = self.pre_feedforward_layernorm(hidden_states)
        hidden_states = self.mlp(hidden_states)
        hidden_states = self.post_feedforward_layernorm(hidden_states)
        hidden_states = residual + self.dropout(hidden_states)
        return hidden_states

# PianistTransformer 747df2d12291e37f6638b39f1b71517e579ad48c, Apache-2.0
class PianoEncoderEmbeddings(nn.Module):
    def __init__(self, config):
        super().__init__()
        self.word_embeddings = nn.Embedding(config.vocab_size, config.hidden_size, padding_idx=config.pad_token_id)
        if config.hidden_size % 8 != 0:
            raise ValueError("Invalid hidden size: must be a multiple of 8.")
        self.projection_layers = nn.ModuleList([nn.Linear(config.hidden_size, config.hidden_size // 8) for i in range(8)])
        self.hidden_size = config.hidden_size

    def forward(
        self,
        input_ids: Optional[torch.LongTensor] = None,
    ) -> torch.Tensor:
        input_shape = input_ids.size()

        batch_size = input_shape[0]
        seq_length = input_shape[1]

        inputs_embeds = self.word_embeddings(input_ids)
        grouped_embeds = inputs_embeds.view(batch_size, seq_length // 8, 8, -1)
        projection_list = []
        for i in range(8):
            projection_list.append(self.projection_layers[i](grouped_embeds[:,:,i,:]))
        projection_cat = torch.cat(projection_list, dim=-1)
        inputs_embeds = projection_cat.view(batch_size, -1, self.hidden_size)
        embeddings = inputs_embeds
        return embeddings
