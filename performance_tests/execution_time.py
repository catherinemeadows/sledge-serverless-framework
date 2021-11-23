import os
import time
import matplotlib.pyplot as plt
from statistics import mean

def light_functions():
    # light function simple fibonacci
    light_arr = []
    for i in range (0,5):
        start_time = time.time()
        os.system ("echo '1' | http :10000")
        finish_time = time.time() - start_time
        light_arr.append(finish_time)
    return light_arr

#def heavy_functions():
    # metrics for heavy functions
 #   return # dict


def plot_all(light_arr):
    # get average to plot
    light_avg = []
    light_avg.append(mean(light_arr))
    print(light_avg)
    labels = []
    labels.append("Fibonacci(1)")
    plt.bar(labels, light_avg)
    plt.title("Lightweight Functions")
    plt.xlabel("Functions")
    plt.ylabel("seconds")
    plt.show()
    plt.savefig('light_function.png')

def main():
    print("Starting...")
    light_arr = light_functions()
    #heavy = heavy_functions()
    plot_all(light_arr)
    print("Done!")

if __name__ == "__main__":
    main()
