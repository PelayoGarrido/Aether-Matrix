"""Fixed-checkpoint, inference-only Pianist Transformer CPU implementation.

Adapted from yhj137/PianistTransformer (747df2d...) and Hugging Face
Transformers v4.54.0 T5Gemma, both Apache-2.0. See THIRD_PARTY_NOTICES.md.
Only the pinned 10/2-layer checkpoint and <=4096 decoder tokens are supported.
No pickle, remote code, hub connection, training or arbitrary architecture.
"""
import math
import torch
import torch.nn.functional as F
from safetensors.torch import load_file


class Pianist:
    def __init__(self, path):
        self.w = {k: v.float() for k, v in load_file(path, device="cpu").items()}
        self.used = set()
        self.cross = []
        self.cache = [None, None]
        self.position = 0
        self.inv_freq = 1.0 / (10000 ** (torch.arange(0, 128, 2).float() / 128))

    def weight(self, key):
        self.used.add(key)
        return self.w[key]

    def linear(self, x, prefix, bias=False):
        return F.linear(x, self.weight(prefix + ".weight"), self.weight(prefix + ".bias") if bias else None)

    def norm(self, x, prefix):
        return (x * torch.rsqrt(x.square().mean(-1, keepdim=True) + 1e-6)) * (1 + self.weight(prefix + ".weight"))

    def rope(self, x, positions):
        angles = positions.float()[:, None] * self.inv_freq[None, :]
        angles = torch.cat((angles, angles), dim=-1)[None, None, :, :]
        rotated = torch.cat((-x[..., 64:], x[..., :64]), dim=-1)
        return x * angles.cos() + rotated * angles.sin()

    def heads(self, x, prefix, kind):
        return self.linear(x, prefix + "." + kind + "_proj").view(1, -1, 8 if kind == "q" else 4, 128).transpose(1, 2)

    def attention(self, x, prefix, positions=None, cache_index=None, cross=None, causal=False):
        q = self.heads(x, prefix, "q")
        if cross is None:
            k, v = self.heads(x, prefix, "k"), self.heads(x, prefix, "v")
            q, k = self.rope(q, positions), self.rope(k, positions)
            if cache_index is not None:
                previous = self.cache[cache_index]
                if previous is not None:
                    k, v = torch.cat((previous[0], k), dim=2), torch.cat((previous[1], v), dim=2)
                self.cache[cache_index] = (k, v)
        else:
            k, v = cross
        k, v = k.repeat_interleave(2, dim=1), v.repeat_interleave(2, dim=1)
        scores = 50 * torch.tanh((q @ k.transpose(-1, -2)) * (256 ** -0.5) / 50)
        if causal:
            query_position = torch.arange(k.shape[2] - q.shape[2], k.shape[2])[:, None]
            mask = torch.arange(k.shape[2])[None, :] > query_position
            scores = scores.masked_fill(mask[None, None, :, :], torch.finfo(scores.dtype).min)
        output = (scores.softmax(dim=-1) @ v).transpose(1, 2).contiguous().view(1, -1, 1024)
        return self.linear(output, prefix + ".o_proj")

    def mlp(self, x, prefix):
        return self.linear(F.gelu(self.linear(x, prefix + ".gate_proj"), approximate="tanh") * self.linear(x, prefix + ".up_proj"), prefix + ".down_proj")

    def encode(self, tokens):
        if not tokens or len(tokens) % 8 or len(tokens) > 4096:
            raise ValueError("Expected 1..512 complete score-note groups")
        self.cache, self.cross, self.position = [None, None], [], 0
        ids = torch.tensor(tokens).view(1, -1)
        embedded = F.embedding(ids, self.weight("model.encoder.embeddings.word_embeddings.weight")).view(1, -1, 8, 768)
        x = torch.cat([self.linear(embedded[:, :, i, :], f"model.encoder.embeddings.projection_layers.{i}", bias=True) for i in range(8)], dim=-1) * math.sqrt(768)
        positions = torch.arange(x.shape[1])
        for i in range(10):
            p = f"model.encoder.layers.{i}"
            x = x + self.norm(self.attention(self.norm(x, p + ".pre_self_attn_layernorm"), p + ".self_attn", positions), p + ".post_self_attn_layernorm")
            x = x + self.norm(self.mlp(self.norm(x, p + ".pre_feedforward_layernorm"), p + ".mlp"), p + ".post_feedforward_layernorm")
        x = self.norm(x, "model.encoder.norm")
        for i in range(2):
            p = f"model.decoder.layers.{i}.cross_attn"
            self.cross.append((self.heads(x, p, "k"), self.heads(x, p, "v")))
        return x

    def decode(self, tokens):
        if not tokens or self.position + len(tokens) > 4096:
            raise ValueError("Decoder context budget exceeded")
        x = F.embedding(torch.tensor(tokens).view(1, -1), self.weight("model.decoder.embed_tokens.weight")) * math.sqrt(768)
        positions = torch.arange(self.position, self.position + len(tokens))
        for i in range(2):
            p = f"model.decoder.layers.{i}"
            x = x + self.norm(self.attention(self.norm(x, p + ".pre_self_attn_layernorm"), p + ".self_attn", positions, i, causal=True), p + ".post_self_attn_layernorm")
            x = x + self.norm(self.attention(self.norm(x, p + ".pre_cross_attn_layernorm"), p + ".cross_attn", cross=self.cross[i]), p + ".post_cross_attn_layernorm")
            x = x + self.norm(self.mlp(self.norm(x, p + ".pre_feedforward_layernorm"), p + ".mlp"), p + ".post_feedforward_layernorm")
        self.position += len(tokens)
        x = self.norm(x, "model.decoder.norm")
        # The checkpoint omits the tied LM head; decoder embeddings are its weights.
        logits = F.linear(x[:, -1, :], self.weight("model.decoder.embed_tokens.weight"))
        return 30 * torch.tanh(logits[0] / 30)

    def check_used_weights(self):
        # Upstream saves a second unused embedding block in the outer model and
        # standard encoder embeddings. All actual transformer weights must run.
        unused = set(self.w) - self.used
        allowed = {k for k in self.w if k.startswith("embeddings.") or k == "model.encoder.embed_tokens.weight"}
        if unused != allowed:
            raise ValueError("Checkpoint contains unexpected or unused inference weights")
