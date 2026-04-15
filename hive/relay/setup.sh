#!/bin/bash
# AeonHive Relay Node Setup Script 
# Intended for Hetzner CX11 (Ubuntu 22.04 LTS)

echo "Starting AeonHive Relay Provisioning..."

# 1. System Updates & Docker
sudo apt-get update && sudo apt-get upgrade -y
sudo apt-get install -y ca-certificates curl gnupg ufw

# Install Docker
sudo install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
sudo chmod a+r /etc/apt/keyrings/docker.gpg
echo \
  "deb [arch="$(dpkg --print-architecture)" signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu \
  "$(. /etc/os-release && echo "$VERSION_CODENAME")" stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
sudo apt-get update
sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin

# 2. Firewall configuration (Allow HTTP/HTTPS + Iroh specific ports)
# Note: Iroh heavily leverages UDP hole punching and QUIC, we open wide range for GossipSub
sudo ufw allow 22/tcp
sudo ufw allow 80/tcp
sudo ufw allow 443/tcp
sudo ufw allow 11204/udp
sudo ufw --force enable

# 3. Create persistent directories
mkdir -p ./data/iroh
mkdir -p ./data/cdn
sudo chown -R 1000:1000 ./data/iroh  # Ensure Iroh has permission to write DHT keys

# 4. Generate dummy nginx.conf if it does not exist
if [ ! -f "nginx.conf" ]; then
cat << 'EOF' > nginx.conf
events {}
http {
    server {
        listen 80;
        location / {
            root /usr/share/nginx/html;
            try_files $uri $uri/ =404;
        }
    }
}
EOF
fi

# 5. Start the swarm
echo "Pulling images and starting services..."
sudo docker compose up -d

echo ""
echo "AeonHive Relay successfully deployed!"
echo "Iroh DHT node is listening."
echo "Nginx fallback is serving ./data/cdn on port 80."
