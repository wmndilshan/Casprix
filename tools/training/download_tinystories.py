"""
Download TinyStories dataset from HuggingFace
"""

try:
    from datasets import load_dataset
    import os
    
    print("=" * 50)
    print("  Downloading TinyStories Dataset")
    print("=" * 50)
    print()
    
    # Create data directory
    os.makedirs("data", exist_ok=True)
    
    print("Loading dataset from HuggingFace...")
    ds = load_dataset("roneneldan/TinyStories")
    
    print(f"✓ Loaded {len(ds['train'])} training stories")
    print(f"✓ Loaded {len(ds['validation'])} validation stories")
    
    # Save to text file
    print("\nSaving to data/tinystories.txt...")
    with open("data/tinystories.txt", "w", encoding="utf-8") as f:
        for i, story in enumerate(ds['train']):
            f.write(story['text'] + "\n\n")
            if (i + 1) % 10000 == 0:
                print(f"  Processed {i + 1} stories...")
    
    print(f"\n✓ Saved {len(ds['train'])} stories")
    
    # Save validation set
    print("\nSaving validation to data/tinystories_val.txt...")
    with open("data/tinystories_val.txt", "w", encoding="utf-8") as f:
        for story in ds['validation']:
            f.write(story['text'] + "\n\n")
    
    print(f"✓ Saved {len(ds['validation'])} validation stories")
    
    # Statistics
    total_chars = sum(len(story['text']) for story in ds['train'])
    print(f"\n" + "=" * 50)
    print(f"  Dataset Statistics")
    print(f"=" * 50)
    print(f"Total stories: {len(ds['train']):,}")
    print(f"Total characters: {total_chars:,}")
    print(f"Average story length: {total_chars // len(ds['train']):,} chars")
    print()
    
except ImportError:
    print("ERROR: 'datasets' library not found!")
    print("\nPlease install:")
    print("  pip install datasets")
    print("\nOr download manually from:")
    print("  https://huggingface.co/datasets/roneneldan/TinyStories")
    exit(1)
except Exception as e:
    print(f"ERROR: {e}")
    exit(1)
