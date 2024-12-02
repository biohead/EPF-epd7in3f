from flask import Flask, jsonify, send_file
import yaml
import requests
import os
import io
import random
import rawpy
import numpy as np
from PIL import Image,ImageDraw,ImageFont,ImageEnhance,ImageOps
from pillow_heif import register_heif_opener
from datetime import datetime
from watchdog.observers import Observer
from watchdog.events import FileSystemEventHandler
import threading

app = Flask(__name__)


DEFAULT_CONFIG = {
    'immich': {
        'url': 'http://localhost',
        'album': 'default_album',
        'rotation': 0,
        'enhanced': 1.0,
        'contrast': 1.0,
    }
}

current_config = DEFAULT_CONFIG.copy()

# Initialize configuration
url = DEFAULT_CONFIG['immich']['url']
albumname = DEFAULT_CONFIG['immich']['album']
rotationAngle = DEFAULT_CONFIG['immich']['rotation']
img_enhanced = DEFAULT_CONFIG['immich']['enhanced']
img_contrast = DEFAULT_CONFIG['immich']['contrast']

# Retrieve environment variables with error handling
apikey = os.getenv('IMMICH_API_KEY')
photodir = os.getenv('IMMICH_PHOTO_DEST', '/photos')
tracking_file = os.path.join(photodir, 'tracking.txt')

# Ensure directory exists
os.makedirs(photodir, exist_ok=True)

# Ensure tracking.txt exists
if not os.path.exists(tracking_file):
    open(tracking_file, 'w').close()

headers = {
    'Accept': 'application/json',
    'x-api-key': apikey
}

# Allowed file extensions
ALLOWED_EXTENSIONS = {'.jpeg', '.raw', '.jpg', '.bmp', '.dng', '.heic', '.arw', '.cr2', '.dng', '.nef', '.raw'}

# Set up the directory for the downloaded images
os.makedirs(photodir, exist_ok=True)
register_heif_opener()


palette = [
    (0, 0, 0),
    (255, 255, 255),
    (255, 243, 56),
    (191, 0, 0),
    (100, 64, 255),
    (67, 138, 28)
]

def load_downloaded_images():
    """ Load downloaded image ID from tracking.txt """
    global albumname
    try:
        # Ensure file exists and is readable/writable
        if not os.path.exists(tracking_file):
            open(tracking_file, 'w').close()
        
        # Ensure file has correct permissions
        os.chmod(tracking_file, 0o666)
        
        with open(tracking_file, 'r+') as f:
            lines = f.readlines()
            
            # If file is empty or first line is not current album name, return empty set
            if not lines or lines[0].strip() != albumname:
                # Rewrite album name
                f.seek(0)
                f.truncate()
                f.write(f"{albumname}\n")
                return set()
            
            # Return all lines except the first as downloaded image IDs
            return set(line.strip() for line in lines[1:] if line.strip())
    except Exception as e:
        print(f"Error reading tracking file: {e}")
        return set()

def save_downloaded_image(asset_id):
    """ Save downloaded image ID from tracking.txt """
    global albumname
    try:
        # Check the file exists and is writable
        if not os.path.exists(tracking_file):
            open(tracking_file, 'w').close()
        
        # Check the permission of the file
        os.chmod(tracking_file, 0o666)
        
        with open(tracking_file, 'r+') as f:
            # Read all lines
            lines = f.readlines()
            
            # If file is empty or first line is not current album name, reset file
            if not lines or lines[0].strip() != albumname:
                f.seek(0)
                f.truncate()
                f.write(f"{albumname}\n")
            else:
                f.seek(0, 2)  # Move to the end of the file
            
            # Add new image ID
            f.write(f"{asset_id}\n")
    except PermissionError:
        print(f"Permission denied when writing to {tracking_file}")
    except IOError as e:
        print(f"IO Error when writing to tracking file: {e}")
    except Exception as e:
        print(f"Unexpected error writing to tracking file: {e}")

def reset_tracking_file():
    """Reset tracking.txt file"""
    try:
        open(tracking_file, 'w').close()
    except Exception as e:
        print(f"Error resetting tracking file: {e}")


def depalette_image(pixels, palette):
    palette_array = np.array(palette)
    diffs = np.sqrt(np.sum((pixels[:, :, None, :] - palette_array[None, None, :, :]) ** 2, axis=3))
    indices = np.argmin(diffs, axis=2)
    indices[indices > 3] += 1  # Simulate the code from the C
    return indices

def scale_img_in_memory(image, target_width=800, target_height=480, bg_color=(255, 255, 255)):
    """
    Process image in memory, return BytesIO object

    :param image: PIL Image object
    :param target_width: width of epaper
    :param target_height: height of epaper
    :param bg_color: background color
    :param rotation: rotation angle (0, 90, 180, 270)
    :return: BytesIO object
    """

    # Update the angle
    rotation = rotationAngle
    # Get data from EXIF
    try:
        exif = image._getexif()
        if exif:
            # EXIF time tag is 36867
            date_time = exif.get(36867)
            if not date_time:
                # Alternative time tag is 306
                date_time = exif.get(306)
        else:
            date_time = None
    except:
        date_time = None

    # Read correct photo orientation from EXIF
    image = ImageOps.exif_transpose(image)
    # Rotate image
    if rotation in [90, 180, 270]:
        image = image.rotate(rotation, expand=True)
    elif rotation not in [0]:
        raise ValueError("Rotation must be 0, 90, 180, or 270 degrees")

    width, height = image.size

    # Calculate scaling ratio
    ratio = min(target_width/width, target_height/height)
    new_width = int(width * ratio)
    new_height = int(height * ratio)

    # Resize the image
    ANTIALIAS = Image.Resampling.LANCZOS if hasattr(Image, 'Resampling') else Image.ANTIALIAS
    img = image.resize((new_width, new_height), ANTIALIAS)

    # Create the background of the image
    output_img = Image.new('RGB', (target_width, target_height), bg_color)

    # calculate position
    paste_x = (target_width - new_width) // 2
    paste_y = (target_height - new_height) // 2

    # Enhance color and contrast
    enhanced_img = ImageEnhance.Color(img).enhance(img_enhanced)
    enhanced_img = ImageEnhance.Contrast(enhanced_img).enhance(img_contrast)
    
    # Palette definition (matching previous quantization logic)
    palette = [
        0, 0, 0,         # Black
        255, 255, 255,   # White
        255, 255, 0,    # Yellow
        255, 0, 0,       # Deep Red
        0, 0, 255,    # Blue
        0, 255, 0      # Green
    ]
    
    # Prepare palette image (similar to previous code)
    e = len(palette)
    assert e > 0, "Palette unexpectedly short"
    assert e <= 768, "Palette unexpectedly long"
    assert e % 3 == 0, "Palette not multiple of 3, so not RGB"

    # Create temporary palette image
    pal_image = Image.new("P", (1, 1))
    
    # Zero-pad palette to 768 values
    palette += (768 - e) * [0]
    pal_image.putpalette(palette)
    
    # Quantize image
    quantized_img = enhanced_img.convert("RGB").quantize(
        palette=pal_image, 
        dither=Image.Dither.FLOYDSTEINBERG
    ).convert("RGB")
    
    output_img.paste(quantized_img, (paste_x, paste_y))
    
    # Add date if available
    if date_time:
        draw = ImageDraw.Draw(output_img)
        try:
            font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 20)
        except:
            font = ImageFont.load_default()
        
        # Format the date
        try:
            try:
                dt = datetime.strptime(date_time, "%Y:%m:%d %H:%M:%S")
                formatted_time = dt.strftime("%Y/%m/%d")
            except ValueError:
                dt = datetime.strptime(date_time, "%Y.%m.%d")
                formatted_time = dt.strftime("%Y/%m/%d")
        except:
            formatted_time = date_time

        def draw_text_with_background(draw, text, font, text_color=(255, 255, 255), bg_color=(0, 0, 0)):
            # Calculate rotated width/height
            if rotation in [90, 270]:
                img_width, img_height = target_height, target_width  # width and height swapped
            else:
                img_width, img_height = target_width, target_height
        
            # Set text position
            if rotation == 0:  # no rotation
                position = (img_width - 200, img_height - 40)
            elif rotation == 90:  # 90 degrees clockwise (actually counterclockwise)
                position = (img_height - 30, 30)
            elif rotation == 180:  # 180 degrees
                position = (img_width -200 , img_height - 40)
            elif rotation == 270:  # 270 degrees clockwise (actually counterclockwise)
                position = (30, img_width - 30)
        
            # Get text bounding box
            text_bbox = draw.textbbox((0, 0), text, font=font)  # use (0, 0) to get text size
            text_width = text_bbox[2] - text_bbox[0]
            text_height = text_bbox[3] - text_bbox[1]
            padding = 5
        
            # Set text position and background rectangle bounds
            if rotation == 0:  # no rotation, bottom right
                position = (img_width - text_width - 40, img_height - text_height - 40)
                rect_coords = [
                    position[0] - padding,  # Top left X
                    position[1] - padding,  # Top left Y
                    position[0] + text_width + padding,  # Bottom right X
                    position[1] + text_height + padding  # Bottom right Y
                ]
            elif rotation == 90:  # 90 degrees, top right
                position = (img_height - text_height - 40, 40)
                rect_coords = [
                    position[0] - padding,  # Top left X
                    position[1] - padding,  # Top left Y
                    position[0] + text_height + padding,  # Bottom right X
                    position[1] + text_width + padding   # Bottom right Y
                ]
            elif rotation == 180:  # 180 degrees, top left
                position = (40, 40)
                rect_coords = [
                    position[0] - padding,  # Top left X
                    position[1] - padding,  # Top left Y
                    position[0] + text_width + padding,  # Bottom right X
                    position[1] + text_height + padding  # Bottom right Y
                ]
            elif rotation == 270:  # 270 degrees, bottom left
                position = (40, img_width - text_width - 40)
                rect_coords = [
                    position[0] - padding,  # Top left X
                    position[1] - padding,  # Top left Y
                    position[0] + text_height + padding,  # Bottom right X
                    position[1] + text_width + padding   # Bottom right Y
                ]
            
            # Draw rectangular background
            draw.rectangle(rect_coords, fill=bg_color)
        
            # Create text based on the rotation of image
            if rotation == 0:
                draw.text(position, text, fill=text_color, font=font)
            else:
                # Create a new image to draw rotated text
                rotated_text = Image.new("RGB", (text_width, text_height), (255, 255, 255))  # white background
                rotated_draw = ImageDraw.Draw(rotated_text)
                rotated_draw.text((0, 0), text, fill=text_color, font=font)
                
                # Rotate text image
                rotated_text = rotated_text.rotate(rotation, expand=True, resample=Image.BICUBIC)
                
                # Calculate where rotated text should be pasted
                if rotation == 90:
                    # 90 degree rotation, display in top right
                    output_img.paste(rotated_text, (position[1], position[0]))
                elif rotation == 180:
                    # 180 degree rotation, display in top left
                    output_img.paste(rotated_text, (position[0], position[1]))
                elif rotation == 270:
                    # 270 degree rotation, display in bottom left
                    output_img.paste(rotated_text, (position[1], position[0]))
                
        # Modify function call
        # draw_text_with_background(draw, formatted_time, font)

        
        # Call improved function
        # draw_text_with_background(draw, formatted_time, font, target_width, target_height)
    
    # Save image into ram
    img_io = io.BytesIO()
    output_img.save(img_io, 'BMP')
    img_io.seek(0)
    
    return img_io

def convert_to_c_code_in_memory(image_data):
    """ Convert image to C code in memory """
    # Convert image data to numpy array
    pixels = np.array(image_data)
    
    # Process palette
    indices = depalette_image(pixels, palette)
    
    # Compress pixels
    height, width = indices.shape
    bytes_array = [
        (indices[y, x] << 4) | indices[y, x + 1] if x + 1 < width else (indices[y, x] << 4)
        for y in range(height)
        for x in range(0, width, 2)
    ]
    
    # Generate C code
    output = io.StringIO()

    for i, byte_value in enumerate(bytes_array):
        output.write(f"{byte_value:02X},")
        if (i + 1) % 16 == 0:
            output.write("\n")
    
    output.write("};\n")
    
    # Convert output to bytes
    result = output.getvalue().encode('utf-8')
    output_bytes = io.BytesIO(result)
    output_bytes.seek(0)
    
    return output_bytes

def convert_raw_or_dng_to_jpg(input_file_path, output_dir):
    """Convert RAW or DNG files to JPG using rawpy."""
    with rawpy.imread(input_file_path) as raw:
        rgb = raw.postprocess(use_camera_wb=True, use_auto_wb=False)
        base_name = os.path.splitext(os.path.basename(input_file_path))[0]
        jpg_file_path = os.path.join(output_dir, f"{base_name}.jpg")
        Image.fromarray(rgb).save(jpg_file_path, 'JPEG')
        return jpg_file_path

def convert_heic_to_jpg(input_file_path, output_dir):
    """Convert heic files to JPG using rawpy."""
    img = Image.open(input_file_path)
    img = img.convert("RGB")
    base_name = os.path.splitext(os.path.basename(input_file_path))[0]
    jpg_file_path = os.path.join(output_dir, f"{base_name}.jpg")
    img.save(jpg_file_path, "JPEG", quality=95)
    # print(f"Successfully converted {input_file_path} to {output_dir}")
    return jpg_file_path

class ConfigFileHandler(FileSystemEventHandler):
    """ Reload configuration and notify application when config.yaml changes """
    def __init__(self, config_path, config_update_callback):
        self.config_path = config_path
        self.config_update_callback = config_update_callback
        self.config = self.load_config()

    def on_modified(self, event):
        if event.src_path == self.config_path:
            print("File modification detected, reloading configuration...")
            new_config = self.load_config()
            # Use callback function to update configuration
            self.config_update_callback(new_config)

    def load_config(self):
        """ Load config """
        if not os.path.exists(self.config_path):
            # If file does not exist, create default configuration file
            print(f"File {self.config_path} does not exist, creating default configuration file...")
            with open(self.config_path, 'w') as file:
                yaml.dump(DEFAULT_CONFIG, file)
            print(f"Default configuration file created: {self.config_path}")
        
        with open(self.config_path, 'r') as file:
            return yaml.safe_load(file)
    
def update_app_config(new_config):
    """ Update global configuration and Flask application configuration """
    global current_config, url, albumname, rotationAngle, img_enhanced, img_contrast
    
    current_config = new_config
    
    # Update Flask application configuration
    app.config['IMMICH_URL'] = new_config['immich']['url']
    app.config['IMMICH_ALBUM'] = new_config['immich']['album']
    app.config['IMMICH_ROTATION'] = new_config['immich']['rotation']
    app.config['IMMICH_ENHANCED'] = new_config['immich']['enhanced']
    app.config['IMMICH_CONTRAST'] = new_config['immich']['contrast']
    
    # Update global variables
    url = new_config['immich']['url']
    albumname = new_config['immich']['album']
    rotationAngle = new_config['immich']['rotation']
    img_enhanced = new_config['immich']['enhanced']
    img_contrast = new_config['immich']['contrast']
    
    print(f"Configuration updated: URL = {url}, Album = {albumname}, angle = {rotationAngle}, enhance = {img_enhanced}, contrast = {img_contrast}")
    
def start_config_watcher(config_path):
    """ Start configuration file monitoring """
    config_handler = ConfigFileHandler(config_path, update_app_config)
    
    # Start monitoring file changes
    observer = Observer()
    observer.schedule(config_handler, path=config_path, recursive=False)
    observer.start()
    
    return observer

def main():
    config_path = '/app/config/config.yaml'
    # Start configuration file monitoring
    config_observer = start_config_watcher(config_path)
    
    try:
        # Initialize configuration
        initial_config = ConfigFileHandler(config_path, update_app_config).config
        update_app_config(initial_config)
        
        # Run Flask application in a separate thread
        app.run(host='0.0.0.0', port=5000, use_reloader=False)
    except KeyboardInterrupt:
        config_observer.stop()
    config_observer.join()

@app.route('/download', methods=['GET'])
def process_and_download():
    
    global url, albumname
    
    # Use current global configuration
    current_url = url
    current_albumname = albumname
    
    try:
        # Check if url and albumname are valid
        if not current_url or not current_albumname:
            return jsonify({"error": "Immich URL or Album not configured"}), 500
            
        # Load list of downloaded images
        downloaded_images = load_downloaded_images()
            
        # Get album list
        response = requests.get(f"{current_url}/api/albums", headers=headers)
        if response.status_code != 200:
            return jsonify({"error": "Failed to fetch albums"}), 500
        
        # Find specified album
        data = response.json()
        albumid = next((item['id'] for item in data if item['albumName'] == current_albumname), None)
        if not albumid:
            return jsonify({"error": "Album not found"}), 404

        # Get photos in the album
        response = requests.get(f"{url}/api/albums/{albumid}", headers=headers)
        if response.status_code != 200:
            return jsonify({"error": "Failed to fetch album details"}), 500

        data = response.json()
        if 'assets' not in data or not data['assets']:
            return jsonify({"error": "No images found in album"}), 404

        # Filter out already downloaded images
        remaining_images = [
            img for img in data['assets'] 
            if img['id'] not in downloaded_images
        ]

        # If all images are downloaded, reset tracking file
        if not remaining_images:
            reset_tracking_file()
            remaining_images = data['assets']

        # Randomly select an undownloaded image
        selected_image = random.choice(remaining_images)
        asset_id = selected_image['id']
        
        # Record downloaded images
        save_downloaded_image(asset_id)

        # Download image to memory
        response = requests.get(f"{url}/api/assets/{asset_id}/original", headers=headers, stream=True)
        if response.status_code != 200:
            return jsonify({"error": "Failed to download image"}), 500

        # Process image in memory
        image_data = io.BytesIO(response.content)
        
        # Process image based on its type
        if selected_image['originalPath'].lower().endswith(('.raw', '.dng', '.arw', '.cr2', '.nef')):
            with rawpy.imread(image_data) as raw:
                rgb = raw.postprocess(use_camera_wb=True, use_auto_wb=False)
                image = Image.fromarray(rgb)
        elif selected_image['originalPath'].lower().endswith('.heic'):
            image = Image.open(image_data).convert("RGB")
        else:
            image = Image.open(image_data)

        # Process image
        processed_image = scale_img_in_memory(image)
        
        # Convert to C code
        processed_image.seek(0)
        c_code = convert_to_c_code_in_memory(Image.open(processed_image))
        
        return send_file(
            c_code,
            mimetype='text/plain',
            as_attachment=True,
            download_name=f"image_{asset_id}.c"
        )

    except Exception as e:
        return jsonify({"error": str(e)}), 500


if __name__ == '__main__':
    main()
    