aperture-model
==============

Tooling for the protocol model that ApertureCoin mining runs
(`doc/pouw-v2.md`, `doc/protocol-model.md`).

- `aperture_model/intops.py`: the integer inference profile `aperture-int-v0`
  (normative).
- `aperture_model/qwen3.py`: the float32 reference, the .apm converter, the
  normative integer forward pass, and the deterministic tiny test model.
- `aperture_model/apm.py`: the .apm container, weights_root and model_id.
- `convert_qwen3.py`: converts a Hugging Face Qwen3-Embedding checkpoint to
  .apm.
- `quality_report.py`: compares the integer model with the float reference.
- `test/test_intprofile.py`: golden vectors that every implementation must
  reproduce.

Install the placeholder model for a testnet node (pinned Hugging Face
revision, SHA-256 checked, converted, model_id checked, installed as
`<datadir>/models/<model_id>.apm`, where the node looks by default):

```
pip install -r requirements.txt
python3 fetch_protocol_model.py [--datadir=~/.aperture]
```

Manual conversion of the placeholder model:

```
pip install -r requirements.txt
mkdir qwen && cd qwen
for f in config.json tokenizer.json model.safetensors; do
  curl -LO https://huggingface.co/Qwen/Qwen3-Embedding-0.6B/resolve/main/$f
done
cd ..
python3 convert_qwen3.py qwen qwen3-embed-0.6b.apm   # expect model_id 036e18a4...
python3 quality_report.py qwen qwen3-embed-0.6b.apm
python3 test/test_intprofile.py
```
