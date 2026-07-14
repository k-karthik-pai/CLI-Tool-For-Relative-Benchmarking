/*
 * Binary Search
 * Repeatedly halves the search space by comparing the middle element.
 * Time complexity: O(log n) | Space: O(1)
 * Requires a SORTED array.
 */

#include <stdio.h>

int binary_search(int arr[], int n, int target) {
    int low = 0, high = n - 1;

    while (low <= high) {
        int mid = low + (high - low) / 2;  // Avoids overflow vs (low+high)/2

        if (arr[mid] == target) {
            return mid;        // Found
        } else if (arr[mid] < target) {
            low = mid + 1;     // Target is in the right half
        } else {
            high = mid - 1;    // Target is in the left half
        }
    }

    return -1;  // Not found
}

int main() {
    // Array must be sorted for binary search to work
    int arr[] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19};
    int n = sizeof(arr) / sizeof(arr[0]);
    int target;

    printf("Sorted array: ");
    for (int i = 0; i < n; i++) {
        printf("%d ", arr[i]);
    }
    printf("\n");

    printf("Enter the number to search: ");
    scanf("%d", &target);

    int result = binary_search(arr, n, target);

    if (result != -1) {
        printf("Found %d at index %d.\n", target, result);
    } else {
        printf("%d not found in the array.\n", target);
    }

    return 0;
}
