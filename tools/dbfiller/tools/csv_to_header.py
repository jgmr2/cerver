#!/usr/bin/env python3
"""One-off conversion of the vendored sample CSVs in data/ into embedded C
headers under src/data/, so the compiled binary has zero runtime file
dependencies. Re-run only if the CSVs in data/ change."""
import os

FILES = [
    ("data/customers-100.csv", "src/data/customers_csv.h", "CUSTOMERS_CSV"),
    ("data/people-100.csv", "src/data/people_csv.h", "PEOPLE_CSV"),
    ("data/organizations-100.csv", "src/data/organizations_csv.h", "ORGANIZATIONS_CSV"),
    ("data/products-100.csv", "src/data/products_csv.h", "PRODUCTS_CSV"),
]


def to_header(csv_path, header_path, var_name):
    with open(csv_path, "r", encoding="utf-8") as f:
        data = f.read()
    with open(header_path, "w", encoding="utf-8") as out:
        out.write(f"/* Generated from {os.path.basename(csv_path)} by tools/csv_to_header.py. Do not edit by hand. */\n")
        out.write(f"#ifndef DBFILLER_{var_name}_H\n#define DBFILLER_{var_name}_H\n\n")
        out.write(f"static const char {var_name}[] =\n")
        for line in data.splitlines():
            escaped = line.replace("\\", "\\\\").replace('"', '\\"')
            out.write(f'    "{escaped}\\n"\n')
        out.write(";\n\n#endif\n")


if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    for csv_path, header_path, var_name in FILES:
        os.makedirs(os.path.dirname(header_path), exist_ok=True)
        to_header(csv_path, header_path, var_name)
        print(f"{csv_path} -> {header_path}")
