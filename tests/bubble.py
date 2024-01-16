from timeit import default_timer as timer
import random


def bubblesort(arr):
    for i in range(len(arr)):
        for j in range(len(arr) - i - 1):
            if arr[j] > arr[j + 1]:
                arr[j], arr[j + 1] = arr[j + 1], arr[j]


def gen_arr(n):
    return [random.randrange(n) for _ in range(n)]


n = int(input())
arr = gen_arr(n)
start = timer()
bubblesort(arr)
end = timer()
print(end - start)
