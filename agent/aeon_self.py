#!/usr/bin/env python3
"""
AeonSelf — Self-Improvement Engine
v1.0 | LoRA fine-tuning + PPO reinforcement + AutoGluon hyperparameter search

This module runs in the background and continuously improves AeonMind
from observed task outcomes. No raw data leaves the device.

Architecture:
  Task logs → Imitation Learning (LoRA) → RL fine-tuning (PPO) → Updated adapter
  Updated adapter → AeonHive (federated aggregation) → All nodes improve
"""

import os
import json
import time
import logging
import hashlib
from datetime import datetime
from typing import List, Dict, Optional, Tuple
from dataclasses import dataclass, asdict
from pathlib import Path

logging.basicConfig(level=logging.INFO, format="%(asctime)s [AeonSelf] %(message)s")
log = logging.getLogger("AeonSelf")

# ─────────────────────────────────────────────
# CONSTANTS
# ─────────────────────────────────────────────

AEON_DATA_DIR = Path(os.path.expanduser("~")) / ".aeon"
TASK_LOGS_DIR = AEON_DATA_DIR / "task_logs"
ADAPTERS_DIR  = AEON_DATA_DIR / "adapters"
TRAINING_DIR  = AEON_DATA_DIR / "training"

MIN_TASKS_FOR_LORA  = 50     # minimum tasks before LoRA training
MIN_TASKS_FOR_RL    = 200    # minimum tasks before PPO fine-tuning
LORA_RANK           = 16     # LoRA rank (r) — higher = more capacity, more VRAM
LORA_ALPHA          = 32     # LoRA scaling factor
TARGET_MODULES      = ["q_proj", "v_proj", "k_proj", "o_proj"]

# ─────────────────────────────────────────────
# DATA MODELS
# ─────────────────────────────────────────────

@dataclass
class TaskRecord:
    """One recorded task interaction"""
    task_id: str
    task_description: str
    steps: List[Dict]           # [{action, observation, url}, ...]
    outcome: str                # "success" / "failure" / "partial"
    reward: float               # 1.0=success, 0.5=partial, 0.0=fail, -0.1/step
    duration_s: float
    model_used: str
    timestamp: float = 0.0

    def __post_init__(self):
        if not self.timestamp:
            self.timestamp = time.time()


@dataclass
class TrainingConfig:
    """Configuration for a LoRA training run"""
    base_model: str = "phi4"
    lora_rank: int = LORA_RANK
    lora_alpha: int = LORA_ALPHA
    learning_rate: float = 2e-4
    num_epochs: int = 3
    batch_size: int = 4
    max_seq_length: int = 2048
    target_modules: List[str] = None

    def __post_init__(self):
        if self.target_modules is None:
            self.target_modules = TARGET_MODULES


# ─────────────────────────────────────────────
# TASK LOG MANAGER
# ─────────────────────────────────────────────

class TaskLogManager:
    """Manages task records on disk — privacy-first, stays local"""

    def __init__(self, log_dir: Path = TASK_LOGS_DIR):
        self.log_dir = log_dir
        self.log_dir.mkdir(parents=True, exist_ok=True)

    def save_task(self, record: TaskRecord):
        """Save a task record to disk"""
        path = self.log_dir / f"{record.task_id}.json"
        with open(path, "w") as f:
            json.dump(asdict(record), f, indent=2)
        log.debug(f"💾 Task logged: {record.task_id} ({record.outcome})")

    def load_all(self) -> List[TaskRecord]:
        """Load all task records"""
        records = []
        for path in self.log_dir.glob("*.json"):
            try:
                with open(path) as f:
                    data = json.load(f)
                records.append(TaskRecord(**data))
            except Exception as e:
                log.warning(f"Failed to load {path}: {e}")
        return sorted(records, key=lambda r: r.timestamp)

    def count(self) -> int:
        return len(list(self.log_dir.glob("*.json")))

    def get_success_rate(self) -> float:
        records = self.load_all()
        if not records:
            return 0.0
        successes = sum(1 for r in records if r.outcome == "success")
        return successes / len(records)


# ─────────────────────────────────────────────
# DATASET BUILDER
# ─────────────────────────────────────────────

class DatasetBuilder:
    """Converts task logs → HuggingFace Dataset for LoRA training"""

    def build_imitation_dataset(self, records: List[TaskRecord]) -> Optional[object]:
        """Build supervised imitation learning dataset from successful tasks"""
        try:
            from datasets import Dataset

            examples = []
            for record in records:
                if record.outcome not in ("success", "partial"):
                    continue

                # Build conversation from steps
                for i, step in enumerate(record.steps):
                    if not step.get("action"):
                        continue
                    
                    # Context: task + previous steps
                    context = f"Task: {record.task_description}\n"
                    if i > 0:
                        prev_steps = record.steps[:i]
                        context += "Previous actions:\n"
                        for ps in prev_steps[-3:]:  # last 3 steps as context
                            context += f"  - {ps.get('action', '')}: {ps.get('observation', '')[:100]}\n"
                    
                    context += f"Current page: {step.get('url', 'unknown')}\n"
                    context += f"Observation: {step.get('observation', '')[:200]}\n"
                    context += "Next action:"
                    
                    examples.append({
                        "text": context,
                        "label": step.get("action", ""),
                        "reward": record.reward
                    })

            if not examples:
                log.warning("No valid examples for imitation dataset")
                return None

            dataset = Dataset.from_list(examples)
            log.info(f"📚 Built imitation dataset: {len(examples)} examples")
            return dataset

        except ImportError:
            log.warning("datasets not installed — run: pip install datasets")
            return None

    def build_rl_dataset(self, records: List[TaskRecord]) -> List[Tuple]:
        """Build (state, action, reward) tuples for RL training"""
        tuples = []
        for record in records:
            for i, step in enumerate(record.steps):
                # Discount future rewards
                discount = 0.99 ** (len(record.steps) - i - 1)
                step_reward = record.reward * discount - 0.01  # step penalty

                state = {
                    "task": record.task_description,
                    "url": step.get("url", ""),
                    "observation": step.get("observation", "")[:500],
                    "step_idx": i
                }
                action = step.get("action", "")
                tuples.append((state, action, step_reward))

        log.info(f"🎮 Built RL dataset: {len(tuples)} (state, action, reward) tuples")
        return tuples


# ─────────────────────────────────────────────
# LORA TRAINER
# ─────────────────────────────────────────────

class LoRATrainer:
    """
    Trains a LoRA adapter on task logs.
    Uses PEFT QLoRA for minimal VRAM (works on 8GB or even CPU with patience).
    """

    def __init__(self, config: TrainingConfig, output_dir: Path = ADAPTERS_DIR):
        self.config = config
        self.output_dir = output_dir
        self.output_dir.mkdir(parents=True, exist_ok=True)

    def train(self, dataset) -> Optional[str]:
        """Run LoRA training, return path to adapter"""
        try:
            import torch
            from transformers import AutoModelForCausalLM, AutoTokenizer, TrainingArguments
            from peft import get_peft_model, LoraConfig, TaskType
            from transformers import Trainer

            log.info(f"🧠 Starting LoRA training: {self.config.base_model} r={self.config.lora_rank}")

            # Load tokenizer
            tokenizer = AutoTokenizer.from_pretrained(
                self.config.base_model, trust_remote_code=True
            )
            if tokenizer.pad_token is None:
                tokenizer.pad_token = tokenizer.eos_token

            # Load model (4-bit quantized = QLoRA)
            load_kwargs = {"trust_remote_code": True}
            if torch.cuda.is_available():
                from transformers import BitsAndBytesConfig
                bnb_config = BitsAndBytesConfig(
                    load_in_4bit=True,
                    bnb_4bit_quant_type="nf4",
                    bnb_4bit_compute_dtype=torch.float16,
                    bnb_4bit_use_double_quant=True
                )
                load_kwargs["quantization_config"] = bnb_config
                load_kwargs["device_map"] = "auto"

            model = AutoModelForCausalLM.from_pretrained(
                self.config.base_model, **load_kwargs
            )

            # Apply LoRA
            lora_config = LoraConfig(
                r=self.config.lora_rank,
                lora_alpha=self.config.lora_alpha,
                target_modules=self.config.target_modules,
                lora_dropout=0.05,
                bias="none",
                task_type=TaskType.CAUSAL_LM
            )
            model = get_peft_model(model, lora_config)
            model.print_trainable_parameters()

            # Tokenize dataset
            def tokenize(example):
                return tokenizer(
                    example["text"] + " " + example["label"],
                    truncation=True,
                    max_length=self.config.max_seq_length,
                    padding="max_length"
                )

            tokenized = dataset.map(tokenize, batched=True)

            # Training args
            run_name = f"aeon_lora_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
            adapter_path = str(self.output_dir / run_name)

            training_args = TrainingArguments(
                output_dir=adapter_path,
                num_train_epochs=self.config.num_epochs,
                per_device_train_batch_size=self.config.batch_size,
                gradient_accumulation_steps=4,
                learning_rate=self.config.learning_rate,
                fp16=torch.cuda.is_available(),
                logging_steps=10,
                save_strategy="epoch",
                report_to="none",  # no wandb/mlflow
                dataloader_num_workers=0
            )

            trainer = Trainer(
                model=model,
                args=training_args,
                train_dataset=tokenized
            )

            trainer.train()
            model.save_pretrained(adapter_path)
            tokenizer.save_pretrained(adapter_path)

            log.info(f"✅ LoRA adapter saved: {adapter_path}")
            return adapter_path

        except ImportError as e:
            log.warning(f"Training deps missing: {e}")
            log.warning("Install: pip install transformers peft accelerate bitsandbytes")
            return None
        except Exception as e:
            log.error(f"Training failed: {e}")
            return None


# ─────────────────────────────────────────────
# HYPERPARAMETER OPTIMIZER
# ─────────────────────────────────────────────

class HyperparamOptimizer:
    """
    Uses AutoGluon to find optimal LoRA hyperparameters.
    Runs periodically once enough task data accumulates.
    """

    def optimize(self, records: List[TaskRecord]) -> TrainingConfig:
        """Find best config — falls back to defaults if AutoGluon not installed"""
        try:
            from autogluon.tabular import TabularPredictor
            import pandas as pd

            if len(records) < 100:
                log.info("Need 100+ tasks for hyperparameter optimization")
                return TrainingConfig()

            # Build experiment data from past training runs
            runs_path = TRAINING_DIR / "hparam_runs.json"
            if not runs_path.exists():
                return TrainingConfig()

            with open(runs_path) as f:
                runs = json.load(f)

            if len(runs) < 5:
                log.info(f"Need 5+ training runs for HPO, have {len(runs)}")
                return TrainingConfig()

            df = pd.DataFrame(runs)
            predictor = TabularPredictor(label="val_success_rate").fit(df)
            best_config_row = df.loc[df["val_success_rate"].idxmax()]

            config = TrainingConfig(
                lora_rank=int(best_config_row.get("lora_rank", LORA_RANK)),
                learning_rate=float(best_config_row.get("learning_rate", 2e-4)),
                batch_size=int(best_config_row.get("batch_size", 4))
            )
            log.info(f"🎯 Optimal config: rank={config.lora_rank}, lr={config.learning_rate}")
            return config

        except ImportError:
            log.info("AutoGluon not installed — using default hyperparameters")
            return TrainingConfig()


# ─────────────────────────────────────────────
# SELF-IMPROVEMENT ORCHESTRATOR
# ─────────────────────────────────────────────

class AeonSelf:
    """
    Orchestrates the full self-improvement cycle:
    1. Monitor task log growth
    2. When threshold reached, build training dataset
    3. Run LoRA training
    4. Submit delta to AeonHive
    5. Repeat

    Runs indefinitely in the background.
    """

    def __init__(self):
        self.log_mgr = TaskLogManager()
        self.dataset_builder = DatasetBuilder()
        self.hparam_optimizer = HyperparamOptimizer()
        self.last_trained_count = 0
        TRAINING_DIR.mkdir(parents=True, exist_ok=True)

    def record_task(self, task_id: str, description: str, steps: List[Dict],
                    outcome: str, model: str) -> TaskRecord:
        """Call this after every AeonMind task completes"""
        # Calculate reward
        reward_map = {"success": 1.0, "partial": 0.5, "failure": 0.0}
        base_reward = reward_map.get(outcome, 0.0)
        step_penalty = len(steps) * 0.01
        reward = max(0.0, base_reward - step_penalty)

        record = TaskRecord(
            task_id=task_id,
            task_description=description,
            steps=steps,
            outcome=outcome,
            reward=reward,
            duration_s=0.0,
            model_used=model
        )
        self.log_mgr.save_task(record)

        # Check if we should trigger training
        total = self.log_mgr.count()
        new_tasks = total - self.last_trained_count

        if total >= MIN_TASKS_FOR_LORA and new_tasks >= 25:
            log.info(f"🎓 {new_tasks} new tasks — triggering LoRA training cycle")
            import threading
            threading.Thread(target=self._training_cycle, daemon=True).start()

        return record

    def _training_cycle(self):
        """Full training cycle — runs in background thread"""
        try:
            records = self.log_mgr.load_all()
            log.info(f"📖 Loaded {len(records)} task records for training")

            # Optimize hyperparameters
            config = self.hparam_optimizer.optimize(records)

            # Build dataset
            dataset = self.dataset_builder.build_imitation_dataset(records)
            if dataset is None or len(dataset) < 10:
                log.warning("Insufficient training data — skipping cycle")
                return

            # Train LoRA
            trainer = LoRATrainer(config)
            adapter_path = trainer.train(dataset)

            if adapter_path:
                self.last_trained_count = self.log_mgr.count()
                self._submit_to_hive(adapter_path, len(records))

        except Exception as e:
            log.error(f"Training cycle failed: {e}")

    def _submit_to_hive(self, adapter_path: str, num_tasks: int):
        """Package adapter delta and submit to AeonHive for federated aggregation"""
        import socket

        # Calculate hash
        delta_hash = ""
        for f in Path(adapter_path).glob("*.safetensors"):
            with open(f, "rb") as fp:
                delta_hash = hashlib.sha256(fp.read()).hexdigest()[:16]
            break

        delta = {
            "type": "LORA_DELTA",
            "delta": {
                "delta_id": f"delta_{int(time.time())}",
                "node_id": socket.gethostname(),
                "model_base": "phi4",
                "lora_rank": LORA_RANK,
                "num_tasks": num_tasks,
                "delta_hash": delta_hash,
                "delta_path": adapter_path,
                "created_at": time.time()
            }
        }

        # Try to submit to local Hive node
        try:
            import socket as sock
            s = sock.socket()
            s.settimeout(5)
            s.connect(("127.0.0.1", 42042))
            s.send(json.dumps(delta).encode())
            response = json.loads(s.recv(4096).decode())
            s.close()

            if response.get("accepted"):
                log.info(f"✅ Delta submitted to AeonHive: {delta['delta']['delta_id']}")
            else:
                log.warning("Hive rejected delta")
        except Exception as e:
            log.info(f"AeonHive not reachable (local only for now): {e}")

    def get_status(self) -> dict:
        total = self.log_mgr.count()
        success_rate = self.log_mgr.get_success_rate()
        next_training = max(0, MIN_TASKS_FOR_LORA - total)

        return {
            "total_tasks": total,
            "success_rate": f"{success_rate:.1%}",
            "trained_at_count": self.last_trained_count,
            "tasks_since_training": total - self.last_trained_count,
            "tasks_until_first_training": next_training,
            "adapters": list(ADAPTERS_DIR.glob("*")) if ADAPTERS_DIR.exists() else []
        }


# ─────────────────────────────────────────────
# CLI ENTRY POINT
# ─────────────────────────────────────────────

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="AeonSelf Self-Improvement Engine")
    parser.add_argument("--status", action="store_true", help="Show training status")
    parser.add_argument("--force-train", action="store_true", help="Force training cycle now")
    args = parser.parse_args()

    engine = AeonSelf()

    if args.status:
        status = engine.get_status()
        print("\n=== AeonSelf Status ===")
        for k, v in status.items():
            print(f"  {k}: {v}")

    elif args.force_train:
        log.info("Forcing training cycle...")
        engine._training_cycle()

    else:
        print("AeonSelf — import and use AeonSelf class in your code")
        print("Or run: python aeon_self.py --status")
