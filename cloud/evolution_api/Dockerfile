FROM python:3.12-slim

WORKDIR /app

# Install deps
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy source
COPY main.py .

# Cloud Run sets PORT
ENV PORT=8080
ENV GCP_PROJECT=aeon-browser-build
ENV GCP_REGION=us-east1

EXPOSE 8080

CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8080"]
