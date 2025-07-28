#!/usr/bin/env python3
"""
OSC сообщение отправитель
Отправляет строковое сообщение через OSC протокол
"""

from pythonosc import udp_client
import argparse
import sys

def send_osc_message(ip="127.0.0.1", port=5005, address="/message", message="Hello OSC"):
    """
    Отправляет OSC сообщение
    
    Args:
        ip (str): IP адрес получателя
        port (int): Порт получателя
        address (str): OSC адрес
        message (str): Сообщение для отправки
    """
    try:
        # Создаем клиент
        client = udp_client.SimpleUDPClient(ip, port)
        
        # Отправляем сообщение
        client.send_message(address, message)
        print(f"Отправлено: '{message}' на {ip}:{port}{address}")
        
    except Exception as e:
        print(f"Ошибка отправки: {e}")
        return False
    
    return True

def main():
    parser = argparse.ArgumentParser(description='Отправка OSC сообщений')
    parser.add_argument('--ip', default='127.0.0.1', help='IP адрес (по умолчанию: 127.0.0.1)')
    parser.add_argument('--port', type=int, default=5005, help='Порт (по умолчанию: 5005)')
    parser.add_argument('--address', default='/message', help='OSC адрес (по умолчанию: /message)')
    parser.add_argument('--message', default='Hello OSC', help='Сообщение для отправки')
    
    args = parser.parse_args()
    
    success = send_osc_message(args.ip, args.port, args.address, args.message)
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()