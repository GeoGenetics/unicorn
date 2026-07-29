import urllib.request
import json

url = "http://localhost:8001/v1/chat/completions"
model = "deepseek-ai/DeepSeek-R1-Distill-Qwen-14B"

print("\n=========================================")
print("  Connected to Cluster Multi-Hop Tunnel!")
print("=========================================\n")

messages = []

while True:
    user_input = input("You: ")
    if user_input.lower().strip() == 'exit':
        break
    if not user_input.strip():
        continue
        
    messages.append({"role": "user", "content": user_input})
    data = json.dumps({"model": model, "messages": messages, "temperature": 0.0}).encode('utf-8')
    req = urllib.request.Request(url, data=data, headers={'Content-Type': 'application/json'})
    
    try:
        with urllib.request.urlopen(req) as response:
            res = json.loads(response.read().decode('utf-8'))
            reply = res['choices'][0]['message']['content']
            print(f"\nAI: {reply}\n")
            messages.append({"role": "assistant", "content": reply})
    except Exception as e:
        print(f"\nTunnel Link Error: {e}\n")
