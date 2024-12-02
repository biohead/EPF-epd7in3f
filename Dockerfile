# 使用 Python 基礎映像
FROM python:3.9-slim

# 複製 Flask 應用和腳本到容器中
COPY . /app/

# 設置工作目錄
WORKDIR /app

# 複製需求文件並安裝依賴
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# 曝露 Flask 端口
EXPOSE 5000

# 環境變數
# IMMICH API KEY
ENV IMMICH_API_KEY="your-api-key"
ENV PATH=/home/app/.local/bin:$PATH

# 啟動 Flask 應用和 Python 腳本
CMD ["python", "app.py"]