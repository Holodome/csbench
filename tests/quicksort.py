from timeit import default_timer as timer
import random

def quicksort(arr):
    if len(arr) <= 1:
        return arr

    pivot = arr[0]
    left = [x for x in arr[1:] if x < pivot]
    right = [x for x in arr[1:] if x >= pivot]
    return quicksort(left) + [pivot] + quicksort(right)


def gen_arr(n):
    return [random.randrange(n) for _ in range(n)]


n = int(input())
arr = gen_arr(n)
start = timer()
quicksort(arr)
end = timer()
print(end - start)
