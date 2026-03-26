FROM python:3.12-slim

LABEL maintainer="DelgadoLogic <build@delgadologic.com>"
LABEL description="Aeon Autonomous Evolution Engine — Research + Vote + Patch"

WORKDIR /app

# System deps
RUN apt-get update && apt-get install -y --no-install-recommends \
    git curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Python deps
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy evolution agents
COPY aeon/evolution/ ./evolution/
COPY aeon/shield/ ./shield/

# Environment (all overridable at deploy time)
ENV SCAN_INTERVAL_SECONDS=3600
ENV SILENCE_CPU_THRESHOLD=15.0
ENV SILENCE_IDLE_SECONDS=300
ENV GCP_PROJECT=aeon-browser-build
ENV PORT=8080

# Health check for Cloud Run
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:${PORT}/health || exit 1

EXPOSE ${PORT}

# Default: run the vote coordinator (also imports research agent)
CMD ["python3", "-m", "uvicorn", "evolution.aeon_vote_coordinator:app", \
     "--host", "0.0.0.0", "--port", "8080", "--workers", "1"]
