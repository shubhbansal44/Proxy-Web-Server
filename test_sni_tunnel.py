import socket
import ssl
import threading
import time

# Dummy python test server that checks SNI (the target server)
def start_secure_server(port):
    context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    # We will just accept the connection
    context.generate_self_signed_cert = True
    
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.bind(('127.0.0.1', port))
    server_sock.listen(1)
    
    def handle():
        conn, addr = server_sock.accept()
        # This will fail since server socket has no cert, so let's just make a simple TCP check
        # Actually it's easier to mock a standard HTTP response in the proxy.
        conn.close()
        server_sock.close()
        
    t = threading.Thread(target=handle)
    t.start()
    return server_sock

# Let's just create an SSL connection to the proxy acting as end-server
# First, run proxy!
