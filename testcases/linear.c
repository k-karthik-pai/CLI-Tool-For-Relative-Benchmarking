/*
 * Linear Search
 * Sequentially checks each element until the target is found.
 * Time complexity: O(n) | Space: O(1)
 * Works on unsorted arrays.
 */

#include <stdio.h>

int linear_search(int arr[], int n, int target) {
    for (int i = 0; i < n; i++) {
        if (arr[i] == target) {
            return i;  // Return index if found
        }
    }
    return -1;  // Return -1 if not found
}

int main() {
    int arr[] = {5, 3, 8, 1, 9, 2, 7, 4, 6};
    int n = sizeof(arr) / sizeof(arr[0]);
    int target;

    printf("Array: ");
    for (int i = 0; i < n; i++) {
        printf("%d ", arr[i]);
    }
    printf("\n");

    printf("Enter the number to search: ");
    scanf("%d", &target);

    int result = linear_search(arr, n, target);

    if (result != -1) {
        printf("Found %d at index %d.\n", target, result);
    } else {
        printf("%d not found in the array.\n", target);
    }

    return 0;
}
