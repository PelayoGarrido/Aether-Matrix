"""Compare the compact runtime against selected original eager Torch layers.

This checks embeddings, every encoder/decoder block and LM logits in float32.
It does not certify the complete upstream framework, BF16 or musical quality.
"""
import json
import math
from pathlib import Path
from types import SimpleNamespace
import torch
from torch import nn
from reference_layers import PianoEncoderEmbeddings, T5GemmaEncoderLayer, T5GemmaDecoderLayer, T5GemmaRMSNorm


def compare(model, tokens, prefix):
    config = json.loads((Path(__file__).parent / "checkpoint-config.json").read_text())
    def module_config(name):
        d = dict(config[name]); d.setdefault("is_decoder", False); d["_attn_implementation"] = "eager"
        return SimpleNamespace(**d)
    def load(module, prefix):
        state = {k[len(prefix):]: v for k, v in model.w.items() if k.startswith(prefix)}
        module.load_state_dict(state, strict=True, assign=True); return module.eval()
    def norm(x, prefix):
        return load(T5GemmaRMSNorm(768, eps=1e-6), prefix)(x)
    def rotary(length):
        # Upstream default RoPE, restricted to this checkpoint's original window.
        inv = 1.0 / (config["encoder"]["rope_theta"] ** (torch.arange(0, 128, 2).float() / 128))
        angles = torch.arange(length).float()[:, None] * inv[None, :]
        angles = torch.cat((angles, angles), -1)[None, :, :]
        return angles.cos(), angles.sin()
    embeddings = load(PianoEncoderEmbeddings(SimpleNamespace(vocab_size=5389, hidden_size=768, pad_token_id=0)), "model.encoder.embeddings.")
    x = embeddings(torch.tensor(tokens).view(1, -1)) * math.sqrt(768)
    positions = rotary(x.shape[1])
    for i in range(10):
        layer = load(T5GemmaEncoderLayer(module_config("encoder"), i), f"model.encoder.layers.{i}.")
        x = layer(x, positions, attention_mask=None)
    encoder = norm(x, "model.encoder.norm.")
    compact_encoder = model.encode(tokens)
    encoder_error = float((encoder - compact_encoder).abs().max())
    x = nn.functional.embedding(torch.tensor(prefix).view(1, -1), model.w["model.decoder.embed_tokens.weight"]) * math.sqrt(768)
    positions = rotary(len(prefix))
    mask = torch.full((len(prefix), len(prefix)), torch.finfo(torch.float32).min).triu(1)[None, None, :, :]
    for i in range(2):
        layer = load(T5GemmaDecoderLayer(module_config("decoder"), i), f"model.decoder.layers.{i}.")
        x = layer(x, positions, attention_mask=mask, encoder_hidden_states=encoder, encoder_attention_mask=None)
    x = norm(x, "model.decoder.norm.")
    logits = nn.functional.linear(x[:, -1, :], model.w["model.decoder.embed_tokens.weight"])[0]
    logits = 30 * torch.tanh(logits / 30)
    compact = model.decode(prefix)
    logit_error = float((logits - compact).abs().max())
    if not torch.isfinite(logits).all() or encoder_error > 0.005 or logit_error > 0.005:
        raise ValueError("Original eager-layer/compact-runtime numerical parity failed")
    return dict(encoder_max_abs_error=encoder_error, logits_max_abs_error=logit_error,
                reference="original eager layers, float32; no padding; <=4096 tokens", prefix_tokens=len(prefix))
