import wmi
import time

w = wmi.WMI(namespace="root\\wmi")
out_path = r"D:\Users\ASUS\Documents\Virtual Machines\shared\cpu.txt"

def get_temp():
    try:
        t = w.MSAcpi_ThermalZoneTemperature()[0]
        return (t.CurrentTemperature / 10.0) - 273.15
    except:
        return None

while True:
    temp = get_temp()
    if temp is not None:
        with open(out_path, "w") as f:
            f.write(str(temp))
    else:
        print("hi")
    time.sleep(1)
