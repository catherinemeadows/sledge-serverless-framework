import os
import time
import matplotlib.pyplot as plt
from statistics import mean

def light_functions():
    # light function simple fibonacci
    light_list = []
    for i in range (0,5):
        start_time = time.time()
        os.system ("echo '1' | http :10000")
        finish_time = time.time() - start_time
        light_list.append(finish_time)
    return light_list

def med_functions():
    # metrics for medium function
    med_list = []
    for i in range(0,5):
        start_time = time.time()
        os.system("echo '10' | http :10000")
        finish_time = time.time() - start_time
        med_list.append(finish_time)
    return med_list

def plot_all(light_list, med_list):
    # get average to plot
    function_avgs = []
    function_avgs.append(mean(light_list))
    function_avgs.append(mean(med_list))

    # plot 
    labels = []
    labels.append("Fibonacci(1), Fibonacci(10)")
    plt.bar(labels, function_avgs)
    plt.title("Sledge - Fibonacci Performance")
    plt.xlabel("Function Calls")
    plt.ylabel("Seconds")
    plt.show()
    plt.savefig('fibonacci_function.png')

def main():
    print("Starting...")
    light_list = light_functions()
    med_list = med_functions()
    plot_all(light_list, med_list)
    print("Done!")

if __name__ == "__main__":
    main()
